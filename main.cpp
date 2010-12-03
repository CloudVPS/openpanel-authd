// This file is part of OpenPanel - The Open Source Control Panel
// OpenPanel is free software: you can redistribute it and/or modify it 
// under the terms of the GNU General Public License as published by the Free 
// Software Foundation, using version 3 of the License.
//
// Please note that use of the OpenPanel trademark may be subject to additional 
// restrictions. For more information, please visit the Legal Information 
// section of the OpenPanel website on http://www.openpanel.com/


#include "authd.h"
#include "version.h"
#include <grace/process.h>
#include <grace/system.h>
#include <grace/tcpsocket.h>
#include <grp.h>

APPOBJECT(AuthdApp);

MetaCache MCache;
AuthdApp *AUTHD;

#define PATH_SWUPD_SOCKET "/var/openpanel/sockets/swupd/swupd.sock"

void handle_SIGTERM (int sig)
{
	AUTHD->shouldRun = false;
}

//  =========================================================================
/// Constructor.
/// Calls daemon constructor, initializes the configdb.
//  =========================================================================
AuthdApp::AuthdApp (void)
	: daemon ("com.openpanel.svc.authd"),
	  conf (this)
{
	shouldRun = true;
	AUTHD = this;
}

//  =========================================================================
/// Destructor.
//  =========================================================================
AuthdApp::~AuthdApp (void)
{
}

//  =========================================================================
/// Main method.
//  =========================================================================
int AuthdApp::main (void)
{
	setgroups (0, NULL);

	if (argv.exists ("--version"))
	{
		fout.writeln ("OpenPanel authd %s" %format (AUTHD_VERSION_FULL));
		return 0;
	}
	string conferr; ///< Error return from configuration class.
	
	// Add watcher value for event log. System will daemonize after
	// configuration was validated.
	conf.addwatcher ("system/eventlog", &AuthdApp::confLog);
	
	// Load will fail if watchers did not valiate.
	if (! conf.load ("com.openpanel.svc.authd", conferr))
	{
		ferr.writeln ("%% Error loading configuration: %s" %format (conferr));
		return 1;
	}
	
	log (log::info, "main    ", "OpenPanel authd %s started", AUTHD_VERSION);
	
	string fname = "/var/openpanel/sockets/authd/authd.sock";
	
	if (fs.exists (fname))
		fs.rm (fname);
	
	SocketGroup socks;
	
	try
	{
		socks.listenTo (fname);
	}
	catch (exception e)
	{
		delayedexiterror ((string) e.description);
		return 1;
	}
	
	delayedexitok ();
	
	fs.chgrp (fname, "openpanel-authd");
	fs.chmod (fname, 0770);
	
	for (int i=0; i<8; ++i)
	{
		new SocketWorker (&socks);
	}
	
	signal (SIGTERM, handle_SIGTERM);
	
	while (shouldRun) sleep (1);

	log (log::info, "main", "Shutting down workers");
	socks.shutdown ();
	
	// clean up the socket
	fs.rm (fname);
	log (log::info, "main", "Shutting down logthread and exiting");
	
	stoplog();
	return 0;
}

//  =========================================================================
/// Configuration watcher for the event log.
//  =========================================================================
bool AuthdApp::confLog (config::action act, keypath &kp,
						const value &nval, const value &oval)
{
	string tstr;
	
	switch (act)
	{
		case config::isvalid:
			// Check if the path for the event log exists.
			tstr = strutil::makepath (nval.sval());
			if (! tstr.strlen()) return true;
			if (! fs.exists (tstr))
			{
				ferr.writeln ("%% Event log path %s does not exist" %format(tstr));
				return false;
			}
			return true;
			
		case config::create:
			// Set the event log target and daemonize.
			addlogtarget (log::file, nval.sval(), 0xff, 1024*1024);
			daemonize(true);
			return true;
	}
	
	return false;
}

// ==========================================================================
// CONSTRUCTOR SocketWorker
// ==========================================================================
SocketWorker::SocketWorker (SocketGroup *grp) : groupthread (*grp)
{
	group = grp;
	spawn ();
}

// ==========================================================================
// DESTRUCTOR SocketWorker
// ==========================================================================
SocketWorker::~SocketWorker (void)
{
}

// ==========================================================================
// METHOD SocketWorker::run
// ==========================================================================
void SocketWorker::run (void)
{
	shouldShutdown = false;
	while (! shouldShutdown)
	{
		tcpsocket s;
		string line;
		
		s = group->accept ();
		if (! s)
		{
			if (! eventqueue()) continue;
			value ev = nextevent();
			if (! ev) continue;
			
			caseselector (ev.type())
			{
				incaseof ("exit") :
					log::write (log::info, "worker", "Shutting down on "
								"request");
					return;
				
				defaultcase :
					log::write (log::warning, "worker", "Received unknown "
								"event of type %S" %format (ev.type()));
					break;
			}
			continue;
		}
		try
		{
			int rounds = 0;
			while (true)
			{
				if (! s.waitforline (line, 1000)) rounds++;
				else break;
				
				if (eventqueue())
				{
					value ev = nextevent();
					if (ev.type() == "exit")
					{
						s.writeln ("-SHUTDOWN");
						s.close ();
						log::write (log::info, "worker", "Shutting down on "
									"request");
						return;
					}
					else if (ev)
					{
						log::write (log::error, "worker", "Unknown event "
									"with type <%S>" %format (ev.type()));
					}
				}
				
				if (rounds > 30)
				{
					s.writeln ("-TIMEOUT");
					s.close ();
					log::write (log::error, "worker", "Timeout");
					break;
				}
			}
			if (rounds > 30) continue;
			if (line.strncmp ("hello ", 6))
			{
				log::write (log::warning, "worker",
							"Bogus greeting: %S" %format (line));
				s.writeln ("-WTF?");
				s.close();
				continue;
			}
			else
			{
				s.writeln ("+OK");
			}
			
			delete line.cutat (' ');
			handler.setModule (line);
			
			handle (s);
			if (handler.transactionid)
			{
				handler.finishTransaction ();
			}
		}
		catch (...)
		{
			log::write (log::error, "worker  ", "Connection closed, rolling "
						"back actions");
			if (handler.transactionid)
				handler.rollbackTransaction ();
		}
		s.close ();
	}
}

// ==========================================================================
// METHOD SocketWorker::handle
// ==========================================================================
void SocketWorker::handle (tcpsocket &s)
{
	bool shouldrun = true;
	
	log::write (log::info, "worker  ", "Handling connection for module <%S>"
				%format (handler.module));
	
	// Keep on going as long as there's stuff to do.
	// Note that we don't catch exceptions here, these will
	// trickle down to the run-method.
	while (shouldrun)
	{
		string line;
		value cmd;
		
		int rounds = 0;
		while (true)
		{
			if (! s.waitforline (line, 1000)) rounds++;
			else break;
			
			if (eventqueue())
			{
				value ev = nextevent();
				if (ev.type() == "exit")
				{
					s.writeln ("-SHUTDOWN");
					s.close ();
					log::write (log::info, "worker", "Shutting down on "
								"request");
					shouldShutdown = true;
					return;
				}
				else if (ev)
				{
					log::write (log::error, "worker", "Unknown event "
								"with type <%S>" %format (ev.type()));
				}
			}
			
			if (rounds > 30) break;
		}
		if (rounds > 30)
		{
			log::write (log::error, "worker", "Timeout on socket");
			s.writeln ("-TIMEOUT");
			throw (1);
		}
		if (line)
		{
			bool cmdok = false;
			bool noerrordata = false;
			bool skipreply = false;
			
			bool tbool;
			value tval;
			
			string errorstr = "Syntax Error";
			int errorcode = 1;
			
			cmd = strutil::splitquoted (line, ' ');
			
			log::write (log::info, "worker", "Command line: %s" %format (line));

			caseselector (cmd[0])
			{
				incaseof ("installfile") :
					if (cmd.count() != 3) break;
					if (handler.installFile (cmd[1], cmd[2])) cmdok = true;
					break;
				
				incaseof ("installuserfile") :
					if (cmd.count() != 4) break;
					if (handler.installUserFile (cmd[1], cmd[2], cmd[3])) cmdok = true;
					break;
				
				incaseof ("deletefile") :
					if (cmd.count() != 2) break;
					if (handler.deleteFile (cmd[1])) cmdok = true;
					break;
				
				incaseof ("deletedir") :
					if (cmd.count() != 2) break;
					if (handler.deleteDir (cmd[1])) cmdok = true;
					break;
				
				incaseof ("makedir") :
					if (cmd.count() !=2) break;
					if (handler.makeDir (cmd[1])) cmdok = true;
					break;
				
				incaseof ("makeuserdir") :
					if (cmd.count() !=4) break;
					if (handler.makeUserDir (cmd[3], cmd[1], cmd[2]))
						cmdok = true;
					break;
				
				incaseof ("createuser") :
					if (cmd.count() != 3) break;
					if (handler.createUser (cmd[1], cmd[2]))
					{
						cmdok = true;
					}
					break;
				
				incaseof ("deleteuser") :
					if (cmd.count() != 2) break;
					if (handler.deleteUser (cmd[1])) cmdok = true;
					break;
					
				incaseof ("setusershell") :
					if (cmd.count() != 3) break;
					if (handler.setUserShell (cmd[1], cmd[2]))
						cmdok = true;
					break;

				incaseof ("setuserpass") :
					if (cmd.count() != 3) break;
					if (handler.setUserPass (cmd[1], cmd[2]))
						cmdok = true;
					break;
					
				incaseof ("setquota") :
					if (cmd.count() != 4) break;
					if (handler.setQuota (cmd[1], cmd[2], cmd[3]))
						cmdok = true;
					break;
					
				incaseof ("startservice") :
					if (cmd.count() != 2) break;
					if (handler.startService (cmd[1])) cmdok = true;
					break;
				
				incaseof ("stopservice") :
					if (cmd.count() != 2) break;
					if (handler.stopService (cmd[1])) cmdok = true;
					break;
				
				incaseof ("reloadservice") :
					if (cmd.count() != 2) break;
					if (handler.reloadService (cmd[1])) cmdok = true;
					break;
				
				incaseof ("setonboot") :
					if (cmd.count() != 3) break;
					tbool = false;
					if (cmd[2] == 1) tbool = true;
					if (handler.setServiceOnBoot (cmd[1], tbool)) cmdok = true;
					break;
				
				incaseof ("runscript") :
					if (cmd.count() < 2) break;
					tval = cmd;
					tval.rmindex (0);
					tval.rmindex (0);
					if (handler.runScriptExt (cmd[1], tval)) cmdok = true;
					break;
				
				incaseof ("runuserscript") :
					if (cmd.count() < 3) break;
					tval = cmd;
					tval.rmindex (0);
					tval.rmindex (0);
					tval.rmindex (0);
					if (handler.runScriptExt (cmd[2], tval, cmd[1])) cmdok = true;
					break;

				incaseof ("rollback") :
					if (cmd.count() > 1) break;
					cmdok = handler.rollbackTransaction ();
					break;
				
				incaseof ("getobject") :
					if (cmd.count() < 2) break;
					cmdok = handler.getObject (cmd[1].sval(), s);
					if (cmdok) skipreply = true;
					break;
					
				incaseof ("osupdate") :
					cmdok = handler.triggerSoftwareUpdate ();
					break;
				
				incaseof ("quit") :
					log::write (log::info, "worker", "Exit on module request");
					cmdok = true;
					shouldrun = false;
					try
					{
						s.writeln ("+OK");
					}
					catch (...) {}
					return;
					
				defaultcase :
					errorstr = "Unknown command";
					noerrordata = true;
					break;
			}
			
			log::write (log::info, "worker  ", "Module=<%S> command=<%S> "
						"status=<%s>" %format (handler.module, cmd[0],
							cmdok ? "OK" : noerrordata ? "UNKNOWN" : "FAIL"));
			
			if (cmdok && (! skipreply)) s.writeln ("+OK");
			else if (! skipreply)
			{
				if (! noerrordata)
				{
					errorstr = handler.lasterror;
					errorcode = handler.lasterrorcode;
				}
				s.writeln ("-ERR:%i:%S" %format (errorcode, errorstr));
				log::write (log::error, "worker", "Error %i: %S"
							%format (errorcode, errorstr));
			}
		}
	}
}

// ==========================================================================
// CONSTRUCTOR SocketGroup
// ==========================================================================
SocketGroup::SocketGroup (void)
{
	shouldShutdown = false;
}

// ==========================================================================
// DESTRUCTOR SocketGroup
// ==========================================================================
SocketGroup::~SocketGroup (void)
{
	//listenSock.o.close();
}

// ==========================================================================
// METHOD SocketGroup::listenTo
// ==========================================================================
void SocketGroup::listenTo (const string &inpath)
{
	exclusivesection (listenSock)
	{
		listenSock.listento (inpath);
	}
}

// ==========================================================================
// METHOD SocketGroup::accept
// ==========================================================================
tcpsocket *SocketGroup::accept (void)
{
	tcpsocket *res = NULL;
	if (shouldShutdown) return NULL;
	
	exclusivesection (listenSock)
	{
		if (! shouldShutdown) res = listenSock.tryaccept (2.5);
	}
	return res;
}

void SocketGroup::shutdown (void)
{
	shouldShutdown = false;
	broadcastevent ("exit");
	while (true)
	{
		gc ();
		if (count()) sleep (1);
		else break;
	}
}

// ==========================================================================
// CONSTRUCTOR CommandHandler
// ==========================================================================
CommandHandler::CommandHandler (void)
{
	transactionid = strutil::uuid ();
}

// ==========================================================================
// DESTRUCTOR CommandHandler
// ==========================================================================
CommandHandler::~CommandHandler (void)
{
	if (module && transactionid)
	{
		value arg;
		arg[0] = transactionid;
		
		runScript ("end-transaction", arg);
	}
}

// ==========================================================================
// METHOD CommandHandler::runScript
// ==========================================================================
bool CommandHandler::runScriptExt (const string &scriptName,
								const value &arguments,
								const string &asUser)
{
	string realUser = asUser;
	if (! guard.checkScriptAccess(module, scriptName, realUser, lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}

	return runScript(scriptName,arguments,realUser);
}

// ==========================================================================
// METHOD CommandHandler::runScript
// ==========================================================================
bool CommandHandler::runScript (const string &scriptName,
								const value &arguments,
								const string &asUser)
{
	static string AlphaNumeric ("abcdefghijklmnopqrstuvwxyz"
								"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
								"0123456789_.-");
	
	string scriptPath;
	value cmdLine;
	
	// Deny any scripts with filename-cooties.
	if (! scriptName.validate (AlphaNumeric))
	{
		lasterrorcode = ERR_INVALID_SCRIPT;
		lasterror     = "Invalid script name";
		return false;
	}

	log::write (log::info, "handler ", "Runscript module=<%S> id=<%S> "
				"name=<%S> argc=<%i>" %format (module, transactionid,
											   scriptName, arguments.count()));
	
	// Fill in the fully qualified path to the script.
	scriptPath = "/var/openpanel/tools/%s" %format (scriptName);
	
	// Croak if the script doesn't exist.
	if (! fs.exists (scriptPath))
	{
		lasterrorcode = ERR_NOT_FOUND;
		lasterror     = "Script file not found";
	}
	
	// Build the argument list for systemprocess.
	cmdLine[0] = scriptPath;
	foreach (arg, arguments)
	{
		cmdLine.newval() = arg;
	}
	
	// Realize the system process.
	systemprocess proc (cmdLine, true, asUser);
	proc.run ();
	
	string line;
	string rdata;
	
	// Get the process output.
	try
	{
		while (! proc.eof ())
		{
			line = proc.read (4096);
			if (line.strlen ()) rdata.strcat (line);
			else break;
		}
	}
	catch (...)
	{
	}
	
	// Close the process and serialize the return value.
	proc.close ();
	proc.serialize ();
	
	// Non-zero return: error condition.
	if (proc.retval ())
	{
		rdata.escape ();
		
		lasterrorcode = ERR_SCRIPT_FAILED;
		lasterror     = rdata;
		return false;
	}
	
	// Everything ok.
	lasterrorcode = 0;
	if (lasterror) lasterror.crop ();
	
	return true;
}

// ==========================================================================
// METHOD CommandHandler::installUserFile
// ==========================================================================
bool CommandHandler::installUserFile (const string &fname, const string &dpath,
									  const string &user)
{
	log::write (log::info, "handler", "installUserFile (%s,%s,%s)"
						%format (fname, dpath, user));
	string pdpath = (dpath[0] == '/') ? dpath.mid(1) : dpath;
	if (dpath.strstr ("..") >= 0)
	{
		lasterrorcode = ERR_POLICY;
		lasterror = "Destination directory contains illegal characters";
		
		log::write (log::error, "handler", "Illegal characters in "
					"makeuserdir argument");
		return false;
	}
	
	string realpath;
	value pw;
	value ugr;
	value gr;
	uid_t destuid = 0;
	gid_t destgid = 0;
	
	gr = kernel.userdb.getgrnam ("openpaneluser");
	if (! gr)
	{
		lasterrorcode = ERR_NOT_FOUND;
		lasterror = "The openpaneluser group was not found";
		
		log::write (log::error, "handler", "No openpaneluser group found");
		return false;
	}
	
	pw = kernel.userdb.getpwnam (user);
	if (! pw)
	{
		lasterrorcode = ERR_NOT_FOUND;
		lasterror = "The user was not found";
		log::write (log::error, "handler", "Unknown user <%S>" %format (user));
		return false;
	}
	
	destuid = pw["uid"].uval();
	destgid = pw["gid"].uval();
	
	ugr = kernel.userdb.getgrgid (destgid);
	if ( ! ugr)
	{
		lasterrorcode = ERR_NOT_FOUND;
		lasterror = "The user's primary group was not found";
		
		log::write (log::error, "handler", "Could not back-resolve gid #%u"
					%format (pw["gid"].uval()));
		return false;
	}
	
	if (! gr["members"].exists (user))
	{
		lasterrorcode = ERR_POLICY;
		lasterror = "The user is not a member of group openpaneluser";
		
		log::write (log::error, "handler", "User <%S> not a member of "
					"group openpaneluser" %format (user));
		return false;
	}
	
	realpath = pw["home"];
	if (realpath[-1] != '/') realpath.strcat ('/');
	realpath.strcat (pdpath);
	
	return installFile (fname, realpath, destuid, destgid);
}

// ==========================================================================
// METHOD CommandHandler::installFile
// ==========================================================================
bool CommandHandler::installFile (const string &fname, const string &_dpath,
								  uid_t destuid, gid_t destgid)
{
	string tfname;
	string tdname;
	value perms;
	string guarderr;
	string dpath = _dpath;
	
	if (dpath.strlen() && (dpath[-1] == '/'))
	{
		dpath.crop (dpath.strlen() - 1);
	}
	
	log::write (log::info, "handler ", "Installfile module=<%S> id=<%S> "
				"name=<%S> dpath=<%S>" %format (module, transactionid,
					fname, dpath));
	
	tfname = guard.translateSource (module, fname, guarderr);
	if (! tfname)
	{
		log::write (log::info, "handler ", "Source policy fail: %s"
						%format (guarderr));
		lasterrorcode = ERR_POLICY;
		lasterror = "Source file name does not match policy: ";
		lasterror.strcat (guarderr);
		return false;
	}
	
	if (! guard.checkDestination (module, fname, dpath, perms, guarderr))
	{
		log::write (log::info, "handler ", "Dest policy fail: %s"
						%format (guarderr));
		lasterrorcode = ERR_POLICY;
		lasterror = "Destination file name does not match policy: ";
		lasterror.strcat (guarderr);
		return false;
	}
	
	tdname = guard.translateDestination (dpath, fname);

	uid_t uid = 0;
	gid_t gid = 0;
	unsigned int mode = 0640;
	
	if (perms.exists ("user"))
	{
		value pw = kernel.userdb.getpwnam (perms["user"].sval());
		if (pw)
		{
			uid = (uid_t) pw["uid"].uval();
			gid = (gid_t) pw["gid"].uval();
		}
	}
	if (perms.exists ("group"))
	{
		value pw = kernel.userdb.getgrnam (perms["group"].sval());
		if (pw)
		{
			gid = (gid_t) pw["gid"].uval();
		}
		else
		{
			log::write (log::error, "handler ", "Cannot find group %s"
							%format (perms["group"]));
			lasterrorcode = ERR_NOT_FOUND;
			lasterror = "Unknown group: ";
			lasterror.strcat (perms["group"].sval());
			return false;
		}
	}
	
	if ( (!uid) && (destuid) ) uid = destuid;
	if ( (!gid) && (destgid) ) gid = destgid;
	
	if (perms.exists ("perms"))
	{
		mode = perms["perms"].sval().toint (8);
	}
	
	value args;
	string modestr;
	modestr.printf ("%o", mode);
	args.newval() = transactionid;
	args.newval() = tfname;
	args.newval() = tdname;
	args.newval() = uid; args[-1].sval();
	args.newval() = gid; args[-1].sval();
	args.newval() = modestr;
	
	return runScript ("install-single-file", args);
}

// ==========================================================================
// METHOD CommandHandler::makeDir
// ==========================================================================
bool CommandHandler::makeDir (const string &_dpath)
{
	string tdname;
	value perms;
	string guarderr;
	string dpath = _dpath;
	
	if (dpath.strlen() && (dpath[-1] == '/'))
	{
		dpath.crop (dpath.strlen() - 1);
	}
	
	log::write (log::info, "handler ", "Makedir module=<%S> id=<%S> "
				"dpath=<%S>" %format (module, transactionid, dpath));
	
	if (! guard.checkDestination (module, "", dpath, perms, guarderr))
	{
		log::write (log::info, "handler ", "Dest policy fail: %s"
						%format (guarderr));
		lasterrorcode = ERR_POLICY;
		lasterror = "Destination directory does not match policy: ";
		lasterror.strcat (guarderr);
		return false;
	}
	
	string fuser = "root";
	string fgroup = "root";
	unsigned int mode = 0600;
	
	if (perms.exists ("user"))
	{
		value pw = kernel.userdb.getpwnam (perms["user"].sval());
		if (pw)
		{
			fuser = perms["user"];
		}
	}
	if (perms.exists ("group"))
	{
		value pw = kernel.userdb.getgrnam (perms["group"].sval());
		if (pw)
		{
			fgroup = perms["group"];
		}
		else
		{
			log::write (log::error, "handler ", "Cannot find group %s"
							%format (perms["group"]));
			lasterrorcode = ERR_NOT_FOUND;
			lasterror = "Unknown group: ";
			lasterror.strcat (perms["group"].sval());
			return false;
		}
	}
	if (perms.exists ("perms"))
	{
		mode = perms["perms"].sval().toint (8);
		if (mode & 0700) mode |= 0100;
		if (mode & 0070) mode |= 0010;
		if (mode & 0007) mode |= 0001;
	}
	
	if (fs.isdir (dpath))
	{
		log::write (log::warning, "handler", "Directory <%s> already "
					"existed when trying to create" %format (dpath));
	}
	else if (!fs.mkdir (dpath))
	{
		log::write (log::error, "handler", "Cannot create dir <%s>"
						%format (dpath));
		return false;
	}

	log::write (log::info, "handler", "Setting up perms: %s/%s %o"
					%format (fuser, fgroup, mode));
	fs.chown (dpath, fuser, fgroup);
	fs.chmod (dpath, mode);
	
	return true;
}

// ==========================================================================
// METHOD CommandHandler::makeUserDir
// ==========================================================================
bool CommandHandler::makeUserDir (const string &dpath,
								  const string &user,
								  const string &modestr)
{
	string pdpath = (dpath[0] == '/') ? dpath.mid(1) : dpath;
	int mode = modestr.toint (8);
	if (dpath.strstr ("..") >= 0)
	{
		lasterrorcode = ERR_POLICY;
		lasterror = "Destination directory contains illegal characters";
		
		log::write (log::error, "handler", "Illegal characters in "
					"makeuserdir argument");
		return false;
	}
	
	string realpath;
	value pw;
	value ugr;
	value gr;
	uid_t destuid;
	gid_t destgid;
	
	gr = kernel.userdb.getgrnam ("openpaneluser");
	if (! gr)
	{
		lasterrorcode = ERR_NOT_FOUND;
		lasterror = "The openpaneluser group was not found";
		
		log::write (log::error, "handler", "No openpaneluser group found");
		return false;
	}
	
	pw = kernel.userdb.getpwnam (user);
	if (! pw)
	{
		lasterrorcode = ERR_NOT_FOUND;
		lasterror = "The user was not found";
		log::write (log::error, "handler", "Unknown user <%S>" %format (user));
		return false;
	}
	
	destuid = pw["uid"].uval();
	destgid = pw["gid"].uval();
	
	ugr = kernel.userdb.getgrgid (destgid);
	if ( ! ugr)
	{
		lasterrorcode = ERR_NOT_FOUND;
		lasterror = "The user's primary group was not found";
		
		log::write (log::error, "handler", "Could not back-resolve gid #%u "
						%format (pw["gid"].uval()));
		return false;
	}
	
	if (! gr["members"].exists (user))
	{
		lasterrorcode = ERR_POLICY;
		lasterror = "The user is not a member of group openpaneluser";
		
		log::write (log::error, "handler", "User <%S> not a member of "
					"group openpaneluser" %format (user));
		return false;
	}
	
	realpath = pw["home"];
	if (realpath[-1] != '/') realpath.strcat ('/');
	realpath.strcat (pdpath);
	
	string tpath = "/";
	
	value elements = strutil::split (realpath, '/');
	foreach (elm, elements)
	{
		if (tpath) tpath.strcat ('/');
		tpath.strcat (elm.sval());
		
		if (! fs.exists (tpath))
		{
			if (! runScript ("make-user-directory",
								$(transactionid)->
								$((unsigned int)destuid)->
								$((unsigned int)destgid)->
								$("%o" %format(mode))->
								$(tpath)))
			{
				lasterror = "Could not create directory";
				
				log::write (log::error, "handler", "Error creating "
							"directory <%S> for user <%S>"
							%format (tpath,user));
				return false;
			}
			
		}
	}
	
	return true;
}

// ==========================================================================
// METHOD CommandHandler::getObject
// ==========================================================================
bool CommandHandler::getObject (const string &objname, file &out)
{
	string fname;
	string err;
	
	fname = guard.translateObject (module, objname, err);
	if (! fname)
	{
		log::write (log::error, "handler", "Cannot find object <%S>: %s"
						%format (objname, err));
		lasterrorcode = ERR_POLICY;
		lasterror = "Object not defined";
		return false;
	}
	
	if (! fs.exists (fname))
	{
		lasterrorcode = ERR_NOT_FOUND;
		lasterror = "Could not find file: ";
		lasterror.strcat (fname);
		return false;
	}
	
	string obj;
	obj = fs.load (fname);
	out.printf ("+OK %i\n", obj.strlen());
	out.puts (obj);
	return true;
}

// ==========================================================================
// METHOD CommandHandler::deleteDir
// ==========================================================================
bool CommandHandler::deleteDir (const string &_dpath)
{
	string tdname;
	value perms;
	string guarderr;
	string dpath = _dpath;
	
	if (dpath.strlen() && (dpath[-1] == '/'))
	{
		dpath.crop (dpath.strlen() - 1);
	}
	
	log::write (log::info, "handler ", "Deletedir module=<%S> id=<%S> "
				"dpath=<%S>" %format (module, transactionid, dpath));
	
	if (! guard.checkDestination (module, "", dpath, perms, guarderr))
	{
		log::write (log::info, "handler ", "Dest policy fail: %s"
					%format (guarderr));
		lasterrorcode = ERR_POLICY;
		lasterror = "Destination directory does not match policy: ";
		lasterror.strcat (guarderr);
		return false;
	}
	
	value inf = fs.getinfo (dpath);
	if (perms.exists ("user"))
	{
		if (perms["user"] != inf["user"])
		{
			log::write (log::error, "handler", "Directory <%S> does not match "
						"ownership policies" %format (dpath));
			lasterrorcode = ERR_POLICY;
			lasterror = "Destination directory ownership mismatch";
			return false;
		}
	}
	if (perms.exists ("group"))
	{
		if (perms["group"] != inf["group"])
		{
			log::write (log::error, "handler", "Directory <%S> does not match "
						"ownership policies" %format (dpath));
			lasterrorcode = ERR_POLICY;
			lasterror = "Destination directory ownership mismatch";
			return false;
		}
		value pw = kernel.userdb.getgrnam (perms["group"].sval());
	}
	
	return runScript ("remove-directory", $(transactionid)->$(dpath));
}

// ==========================================================================
// METHOD CommandHandler::finishTransaction
// ==========================================================================
void CommandHandler::finishTransaction (void)
{
	if (! transactionid) return;
	
	runScript ("end-transaction", $(transactionid));
	
	log::write (log::info, "handler ", "Closing transaction module=<%S> "
				"id=<%S>" %format (module, transactionid));

	transactionid = nokey;
}

// ==========================================================================
// METHOD CommandHandler::rollbackTransaction
// ==========================================================================
bool CommandHandler::rollbackTransaction (void)
{
	if (! transactionid) return false;
	
	log::write (log::info, "handler ", "Rolling back transaction module=<%S> "
				"id=<%S>" %format (module, transactionid));

	return runScript ("rollback-transaction", $(transactionid));
}

// ==========================================================================
// METHOD CommandHandler::deleteFile
// ==========================================================================
bool CommandHandler::deleteFile (const string &path)
{
	log::write (log::info, "handler ", "Delete file module=<%S> id=<%S> "
				"path=<%S>" %format (module, transactionid, path));
	
	string guarderr;
	
	if (! guard.checkDelete (module, path, guarderr))
	{
		lasterrorcode = ERR_POLICY;
		lasterror     = "Destination file name does not match policy: ";
		lasterror.strcat (guarderr);
		return false;
	}
	
	return runScript ("remove-single-file", $(transactionid)->$(path));
}

// ==========================================================================
// METHOD CommandHandler::createUser
// ==========================================================================
bool CommandHandler::createUser (const string &userName, const string &ppass)
{
	static string validUser ("abcdefghijklmnopqrstuvwxyz0123456789_-."
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
	static string validPass ("abcdefghijklmnopqrstuvwxyz0123456789"
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()+="
							 "<>,./?;:'|{}[]-_`~ ");
	
	log::write (log::info, "handler ", "Create user module=<%S> id=<%S> "
				"name=<%S>" %format (module, transactionid, userName));
	
	if (! guard.checkCommandAccess(module, "createuser","user", lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}
	
	if (! userName.validate (validUser))
	{
		lasterrorcode = ERR_POLICY;
		lasterror     = "Invalid username";
		return false;
	}
	
	if (! ppass.validate (validPass))
	{
		lasterrorcode = ERR_POLICY;
		lasterror     = "Invalid password format";
		return false;
	}
	
	value args = $(transactionid)->$(userName)->$(ppass);
	return runScript ("create-system-user", args);
}

// ==========================================================================
// METHOD CommandHandler::deleteUser
// ==========================================================================
bool CommandHandler::deleteUser (const string &userName)
{
	static string validUser ("abcdefghijklmnopqrstuvwxyz0123456789_-."
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

	log::write (log::info, "handler ", "Delete user module=<%S> id=<%S> "
				"name=<%S>" %format (module, transactionid, userName));
	
	if (! guard.checkCommandAccess(module, "deleteuser","user", lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}
	
	if (! userName.validate (validUser))
	{
		lasterrorcode = ERR_POLICY;
		lasterror     = "Invalid username";
		return false;
	}
	
	return runScript ("remove-system-user", $(transactionid)->$(userName));
}

// ==========================================================================
// METHOD CommandHandler::setUserShell
// ==========================================================================
bool CommandHandler::setUserShell (	const string &userName,
							   		const string &shell)
{
	static string validUser ("abcdefghijklmnopqrstuvwxyz0123456789_-."
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

	log::write (log::info, "handler ", "Set User's Shell module=<%S> id=<%S> "
				"name=<%S>" %format (module, transactionid, userName));
	
	if (! guard.checkCommandAccess(module, "setusershell", "user", lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}
	
	if (! userName.validate (validUser))
	{
		lasterrorcode = ERR_POLICY;
		lasterror     = "Invalid username";
		return false;
	}
	
	value args = $(transactionid)->$(userName)->$(shell);
	return runScript ("change-system-usershell", args);
}

// ==========================================================================
// METHOD CommandHandler::setUserShell
// ==========================================================================
bool CommandHandler::setUserPass (	const string &userName,
							   		const string &password)
{
	static string validUser ("abcdefghijklmnopqrstuvwxyz0123456789_-."
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

	log::write (log::info, "handler ", "Set User's Shell module=<%S> id=<%S> "
				"name=<%S>" %format (module, transactionid, userName));
	
	if (! guard.checkCommandAccess(module, "setuserpass", "user", lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}
	
	if (! userName.validate (validUser))
	{
		lasterrorcode = ERR_POLICY;
		lasterror     = "Invalid username";
		return false;
	}
	
	value args = $(transactionid)->$(userName)->$(password);
	return runScript ("change-user-password", args);
}

// ==========================================================================
// METHOD CommandHandler::setQuota
// ==========================================================================
bool CommandHandler::setQuota (const string &userName,
							   unsigned int softLimit,
							   unsigned int hardLimit)
{
	static string validUser ("abcdefghijklmnopqrstuvwxyz0123456789_-."
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

	log::write (log::info, "handler ", "Set User's quota module=<%S> id=<%S> user=<%S> "
				"soft/hard=<%d/%d>" %format (module, transactionid,
											 userName,softLimit, hardLimit));
	
	if (! guard.checkCommandAccess(module, "setquota", "user", lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}

	if (! userName.validate (validUser))
	{
		lasterrorcode = ERR_POLICY;
		lasterror     = "Invalid username";
		return false;
	}
	
	value args = $(transactionid)->$(userName)->$(softLimit)->$(hardLimit);
	return runScript ("change-user-quota", args);
}

// ==========================================================================
// METHOD PathGuard::checkServiceAccess
// ==========================================================================
bool PathGuard::checkServiceAccess (const string &moduleName,
									const string &serviceName,
									string &error)
{
	value meta;
	meta = cache.get (moduleName);
	if (! meta)
	{
		error = "Could not find module";
		log::write (log::error, "srvaccs", "Could not load module <%S>"
						%format (moduleName));
		return false;
	}
	
	if (! meta["authdops"]["services"].exists(serviceName))
	{
		error = "Service not defined in module.xml";
		return false;
	}
	
	return true;
}

// ==========================================================================
// METHOD PathGuard::checkScriptAccess
// ==========================================================================
bool PathGuard::checkScriptAccess (const string &moduleName,
								   const string &scriptName,
								   string &userName,
								   string &error)
{
	value meta;
	
	log::write (log::info, "scraccs ", "Checking script access module=<%S> "
				"script=<%S>" %format (moduleName, scriptName));
				
	meta = cache.get (moduleName);
	if (! meta)
	{
		error = "Could not find module";
		log::write (log::error, "scraccs", "Could not load module <%S>"
					%format (moduleName));
		return false;
	}
	
	if (! meta["authdops"]["scripts"].exists(scriptName))
	{
		log::write (log::error, "scraccs", "Script not defined in "
					"module.xml: <%S>" %format (scriptName));
		error = "Script not defined in module.xml";
		return false;
	}
	
	value &scrip = meta["authdops"]["scripts"][scriptName];
	if (scrip.attribexists ("asroot"))
	{
		if ((scrip("asroot") == false) && (userName == "root"))
		{
			log::write (log::error, "scraccs", "Script <%S> may not be "
						"run as root as per the module.xml for <%S>"
							%format (scriptName, moduleName));
		}
	}
	
	if (scrip.attribexists ("asuser"))
	{
		userName = scrip("asuser");
	}

	log::write (log::info, "scraccs ", "Allowing script access module=<%S> sc"
				"ript=<%S> user=<%S>" %format (moduleName,scriptName,userName));
	
	return true;
}

// ==========================================================================
// METHOD PathGuard::checkCommandAccess
// ==========================================================================
bool PathGuard::checkCommandAccess (const string &moduleName,
								    const string &cmdName,
								    const string &cmdClass,
								    string &error)
{
	value meta;
	
	log::write (log::info, "cmdaccs ", "Checking command access module=<%S> "
				"command=<%S> commandclass=<%S>" %format (moduleName,
					cmdName, cmdClass));
				
	meta = cache.get (moduleName);
	if (! meta)
	{
		error = "Could not find module";
		log::write (log::error, "cmdaccs", "Could not load module <%S>"
						%format (moduleName));
		return false;
	}
	
	if (! meta["authdops"]["commands"].exists(cmdName)
	&&  ! meta["authdops"]["commandclasses"].exists(cmdClass))
	{
		log::write (log::error, "cmdaccs", "Command or command class not "
					"defined in module.xml");
		error = "Command or command class not defined in module.xml";
		return false;
	}
		
	log::write (log::info, "cmdaccs ", "Allowing command access module=<%S> "
					%format (moduleName));
	
	return true;
}

// ==========================================================================
// METHOD CommandHandler::startService
// ==========================================================================
bool CommandHandler::startService (const string &serviceName)
{
	log::write (log::info, "handler ", "Start service module=<%S> id=<%S> "
				"name=<%S>" %format (module, transactionid, serviceName));
	
	if (! guard.checkServiceAccess(module, serviceName, lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}

	return runScript ("control-service", $("start")->$(serviceName));
}

// ==========================================================================
// METHOD CommandHandler::stopService
// ==========================================================================
bool CommandHandler::stopService (const string &serviceName)
{
	log::write (log::info, "handler ", "Stop service module=<%S> id=<%S> "
				"name=<%S>" %format (module, transactionid, serviceName));
	
	if (! guard.checkServiceAccess(module, serviceName, lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}

	return runScript ("control-service", $("stop")->$(serviceName));
}

// ==========================================================================
// METHOD CommandHandler::reloadService
// ==========================================================================
bool CommandHandler::reloadService (const string &serviceName)
{
	log::write (log::info, "handler ", "Reload service module=<%S> id=<%S> "
				"name=<%S>" %format (module, transactionid, serviceName));
	
	if (! guard.checkServiceAccess(module, serviceName, lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}

	return runScript ("control-service", $("reload")->$(serviceName));
}

// ==========================================================================
// METHOD CommandHandler::setServiceOnBoot
// ==========================================================================
bool CommandHandler::setServiceOnBoot (const string &serviceName,
									   bool onBoot)
{
	log::write (log::info, "handler ", "Service onboot module=<%S> id=<%S> "
				"name=<%S> status=<%s>" %format (module, transactionid,
					serviceName, onBoot ? "on" : "off"));
	
	if (! guard.checkServiceAccess(module, serviceName, lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}
	
	return runScript ("control-service-boot", $(serviceName)->$(onBoot?1:0));
}

// ==========================================================================
// METHOD CommandHandler::setServiceOnBoot
// ==========================================================================
void CommandHandler::setModule (const string &moduleName)
{
	module = moduleName;
	transactionid = strutil::uuid();
	
	log::write (log::info, "handler ", "Started transaction module=<%S> "
				"id=<%S>" %format (module, transactionid));
}

// ==========================================================================
// METHOD CommandHandler::triggerSoftwareUpdate
// ==========================================================================
bool CommandHandler::triggerSoftwareUpdate (void)
{
	tcpsocket s;
	
	if (! guard.checkCommandAccess(module, "osupdate", "", lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}
	
	
	if (! s.uconnect (PATH_SWUPD_SOCKET))
	{
		log::write (log::error, "handler", "Could not connect to swupd "
					"socket");
		return false;
	}
	
	try
	{
		s.puts ("update\n");
		string line = s.gets();
		if (! line) line = s.gets();
		s.close ();
		if (line[0] == '+')
		{
			log::write (log::info, "handler", "Triggered software "
						"update");
			return true;
		}
		else
		{
			log::write (log::info, "handler", "Error from swupd: %S"
						%format (line));
		}
	}
	catch (exception e)
	{
		log::write (log::info, "handler", "Exception: %S"
					%format (e.description));
	}
	
	s.close ();
	log::write (log::error, "handler", "Error from swupd");
	return false;
}

// ==========================================================================
// CONSTRUCTOR MetaCache
// ==========================================================================
MetaCache::MetaCache (void)
{
}

// ==========================================================================
// DESTRUCTOR MetaCache
// ==========================================================================
MetaCache::~MetaCache (void)
{
}

// ==========================================================================
// METHOD MetaCache::get
// ==========================================================================
value *MetaCache::get (const statstring &moduleName)
{
	static string AlphaNumeric ("abcdefghijklmnopqrstuvwxyz"
								"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
								"0123456789_.-");
	
	static xmlschema S ("schema:com.openpanel.opencore.module.schema.xml");
	unsigned int NOW = kernel.time.now ();
	
	if (! moduleName.sval().validate (AlphaNumeric))
	{
		return NULL;
	}

	returnclass (value) res retain;

	sharedsection (cache)
	{
		if (cache.exists (moduleName))
		{
			res = cache[moduleName];
			if ((NOW - res("time").uval()) < 60)
			{
				breaksection return &res;
			}
		}
	}
	
	string mxmlpath;
	mxmlpath = "/var/openpanel/modules/%s.module/module.xml" %format (moduleName);
	if (! fs.exists (mxmlpath)) return &res;
	
	res.loadxml (mxmlpath, S);
	res ("time") = NOW;
	
	exclusivesection (cache)
	{
		cache[moduleName] = res;
	}
	
	return &res;
}

// ==========================================================================
// CONSTRUCTOR PathGuard
// ==========================================================================
PathGuard::PathGuard (void) : cache (MCache)
{
}

// ==========================================================================
// DESTRUCTOR PathGuard
// ==========================================================================
PathGuard::~PathGuard (void)
{
}

// ==========================================================================
// METHOD PathGuard::translateSource
// ==========================================================================
string *PathGuard::translateSource (const statstring &moduleName,
								   const string &fileName,
								   string &error)
{
	static string validFileName ("abcdefghijklmnopqrstuvwxyz"
								 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
								 "0123456789.:_-@+ /");

	// Hold the filename to some standards.
	//if (! fileName.validate (validFileName)) return NULL;
	if (fileName[0] == '/')
	{
		error = "Filename starts with a slash";
		return NULL;
	}
	if (fileName.strstr ("..") >= 0)
	{
		error = "Filename contains relative path elements";
		return NULL;
	}

	value meta;
	meta = cache.get (moduleName);
	if (! meta)
	{
		error = "Could not find module";
		log::write (log::error, "pathgrd ", "Could not load module <%S>"
						%format (moduleName));
		return NULL;
	}
	
	returnclass (string) res retain;
	
	foreach (op, meta["authdops"]["fileops"])
	{
		if (fileName.globcmp (op.id().sval()))
		{
			res.printf ("/var/openpanel/conf/staging/%s/%s",
						moduleName.str(), fileName.str());
			
			if (! fs.exists (res))
			{
				error = "Source file does not exist";
				res.crop ();
			}
			else
			{
				unsigned int perms;
				value finf;
				finf = fs.getinfo (res);
				if (finf["user"] != "openpanel-core")
				{
					log::write (log::error, "pathgrd ", "Owner mismatch "
								"on file <%S>: %s" %format (fileName,
															finf["user"]));
					error = "File owner mismatch (not openpanel-core)";
					res.crop();
				}
				else if (finf["group"] != "openpanel-core")
				{
					log::write (log::error, "pathgrd ", "Group mismatch "
								"on file <%S>: %s" %format (fileName,
															finf["group"]));
					error = "File group mismatch (not openpanel-core)";
					res.crop();
				}
				else
				{
					perms = finf["mode"].uval();
					if (perms & 1)
					{
						log::write (log::error, "pathgrd ", "Denied world-"
									"writable file <%S>" %format (fileName));
						error = "File is world-writable";
						res.crop();
					}
				}
				
			}
			return &res;
		}
	}
	
	error = "No matching fileop found in module.xml";
	return &res;
}

// ==========================================================================
// METHOD PathGuard::checkDestination
// ==========================================================================
bool PathGuard::checkDestination (const statstring &moduleName,
								  const string &sourceFile,
								  const string &filePath,
								  value &perms,
								  string &error)
{
	value meta;
	meta = cache.get (moduleName);
	if (! meta) return false;
	
	
	foreach (op, meta["authdops"]["fileops"])
	{
		string opath = op.sval();
		string fpath = filePath;

		if (opath.strlen()>2)
		{
			if ((opath[-1] == '*')&&(opath[-2] == '/'))
			{
				fpath.strcat ("/");
			}
		}
		
		if (! sourceFile)
		{
			if (fpath.globcmp (op.sval()))
			{
				perms = op.attributes();
				return true;
			}
		}
		else if (sourceFile.globcmp (op.id().sval()))
		{
			if (fpath.globcmp (op.sval()))
			{
				perms = op.attributes();
				return true;
			}
		}
	}
	
	error = "No matching destination path found in fileop";
	log::write (log::warning, "pathgrd ", "Denied module=<%S> file=<%S> "
				"destpath=<%S>" %format (moduleName, sourceFile,filePath));
	
	return false;
}

// ==========================================================================
// METHOD PathGuard::translateDestination
// ==========================================================================
string *PathGuard::translateDestination (const string &filePath,
								         const string &sourceFile)
{
	returnclass (string) res retain;
	string tsrc = sourceFile;
	if (tsrc.strchr ('/') >= 0) tsrc = tsrc.cutafterlast ('/');
	
	res = filePath;
	if (res[-1] != '/') res.strcat ('/');
	res.strcat (tsrc);
	return &res;
}

// ==========================================================================
// METHOD PathGuard::translateObject
// ==========================================================================
string *PathGuard::translateObject (const statstring &moduleName,
									const string &name, string &error)
{
	returnclass (string) res retain;
	value meta;
	meta = cache.get (moduleName);
	if (! meta)
	{
		error = "Module not found";
		return &res;
	}
	
	if (! meta["authdops"]["objects"].exists (name))
	{
		error = "File not defined in module.xml";
		return &res;
	}
	
	res = meta["authdops"]["objects"][name];
	return &res;
}

// ==========================================================================
// METHOD PathGuard::checkDelete
// ==========================================================================
bool PathGuard::checkDelete (const statstring &moduleName,
							 const string &fullPath,
							 string &error)
{
	value meta;
	string match;
	meta = cache.get (moduleName);
	if (! meta) return false;
	
	foreach (op, meta["authdops"]["fileops"])
	{
		match = op.sval();
		if (match[-1] != '*')
		{
			if (match[-1] == '/')
				match.strcat ('*');
			else
				match.strcat ("/*");
		}
		if (fullPath.globcmp (match)) return true;
	}
	
	error = "No matching destination path found in any fileop for the module";
	log::write (log::warning, "pathgrd ", "Denied delete module=<%S> "
				"file=<%S>" %format (moduleName, fullPath));
	
	return false;
}
