// --------------------------------------------------------------------------
// OpenPanel - The Open Source Control Panel
// Copyright (c) 2006-2007 PanelSix
//
// This software and its source code are subject to version 2 of the
// GNU General Public License. Please be aware that use of the OpenPanel
// and PanelSix trademarks and the IconBase.com iconset may be subject
// to additional restrictions. For more information on these restrictions
// and for a copy of version 2 of the GNU General Public License, please
// visit the Legal and Privacy Information section of the OpenPanel
// website on http://www.openpanel.com/
// --------------------------------------------------------------------------

#include "authd.h"
#include "version.h"
#include <grace/process.h>
#include <grace/system.h>
#include <grace/tcpsocket.h>
#include <grp.h>

APPOBJECT(authdApp);

metacache MCache;
authdApp *AUTHD;

#define PATH_SWUPD_SOCKET "/var/opencore/sockets/swupd/swupd.sock"

void handle_SIGTERM (int sig)
{
	AUTHD->shouldRun = false;
}

//  =========================================================================
/// Constructor.
/// Calls daemon constructor, initializes the configdb.
//  =========================================================================
authdApp::authdApp (void)
	: daemon ("com.openpanel.svc.authd"),
	  conf (this)
{
	shouldRun = true;
	AUTHD = this;
}

//  =========================================================================
/// Destructor.
//  =========================================================================
authdApp::~authdApp (void)
{
}

//  =========================================================================
/// Main method.
//  =========================================================================
int authdApp::main (void)
{
	setgroups (0, NULL);

	if (argv.exists ("--version"))
	{
		fout.printf ("OpenPanel authd %s\n", AUTHD_VERSION_FULL);
		return 0;
	}
	string conferr; ///< Error return from configuration class.
	
	// Add watcher value for event log. System will daemonize after
	// configuration was validated.
	conf.addwatcher ("system/eventlog", &authdApp::confLog);
	
	// Load will fail if watchers did not valiate.
	if (! conf.load ("com.openpanel.svc.authd", conferr))
	{
		ferr.printf ("%% Error loading configuration: %s\n", conferr.str());
		return 1;
	}
	
	log (log::info, "main    ", "OpenPanel authd %s started", AUTHD_VERSION);
	
	string fname = "/var/opencore/sockets/authd/authd.sock";
	
	if (fs.exists (fname))
		fs.rm (fname);
	
	socketGroup socks;
	
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
	
	fs.chgrp (fname, "authd");
	fs.chmod (fname, 0770);
	
	for (int i=0; i<8; ++i)
	{
		new socketWorker (&socks);
	}
	
	signal (SIGTERM, handle_SIGTERM);
	
	while (shouldRun) sleep (1);

	log (log::info, "main", "Shutting down workers");
	socks.shutdown ();
	log (log::info, "main", "Shutting down logthread and exiting");
	
	stoplog();
	return 0;
}

//  =========================================================================
/// Configuration watcher for the event log.
//  =========================================================================
bool authdApp::confLog (config::action act, keypath &kp,
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
				ferr.printf ("%% Event log path %s does not exist",
							 tstr.str());
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
// CONSTRUCTOR socketWorker
// ==========================================================================
socketWorker::socketWorker (socketGroup *grp) : groupthread (*grp)
{
	group = grp;
	spawn ();
}

// ==========================================================================
// DESTRUCTOR socketWorker
// ==========================================================================
socketWorker::~socketWorker (void)
{
}

// ==========================================================================
// METHOD socketWorker::run
// ==========================================================================
void socketWorker::run (void)
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
					AUTHD->log (log::info, "worker", "Shutting down on "
								"request");
					return;
				
				defaultcase :
					AUTHD->log (log::warning, "worker", "Received unknown "
								"event of type %S", ev.type().str());
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
						AUTHD->log (log::info, "worker", "Shutting down on "
									"request");
						return;
					}
					else if (ev)
					{
						AUTHD->log (log::error, "worker", "Unknown event "
									"with type <%S>", ev.type().str());
					}
				}
				
				if (rounds > 30)
				{
					s.writeln ("-TIMEOUT");
					s.close ();
					AUTHD->log (log::error, "worker", "Timeout");
					break;
				}
			}
			if (rounds > 30) continue;
			if (line.strncmp ("hello ", 6))
			{
				AUTHD->log (log::warning, "worker  ", "Bogus greeting: %s" %format (line));
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
				handler.finishTransaction ();
		}
		catch (...)
		{
			AUTHD->log (log::error, "worker  ", "Connection closed, rolling "
						"back actions");
			if (handler.transactionid)
				handler.rollbackTransaction ();
		}
		s.close ();
	}
}

// ==========================================================================
// METHOD socketWorker::handle
// ==========================================================================
void socketWorker::handle (tcpsocket &s)
{
	bool shouldrun = true;
	
	AUTHD->log (log::info, "worker  ", "Handling connection for module <%S>",
				handler.module.str());
	
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
					AUTHD->log (log::info, "worker", "Shutting down on "
								"request");
					shouldShutdown = true;
					return;
				}
				else if (ev)
				{
					AUTHD->log (log::error, "worker", "Unknown event "
								"with type <%S>", ev.type().str());
				}
			}
			
			if (rounds > 30) break;
		}
		if (rounds > 30)
		{
			AUTHD->log (log::error, "worker  ", "Timeout on socket");
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
			
			AUTHD->log (log::info, "worker", "Command line: %s" %format (line));

			caseselector (cmd[0])
			{
				incaseof ("installfile") :
					if (cmd.count() != 3) break;
					if (handler.installFile (cmd[1], cmd[2])) cmdok = true;
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
					AUTHD->log (log::info, "worker  ", "Exit on module request");
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
			
			AUTHD->log (log::info, "worker  ", "Module=<%S> command=<%S> "
						"status=<%s>", handler.module.str(), cmd[0].str(),
						cmdok ? "OK" : noerrordata ? "UNKNOWN" : "FAIL");
			
			if (cmdok && (! skipreply)) s.writeln ("+OK");
			else if (! skipreply)
			{
				if (! noerrordata)
				{
					errorstr = handler.lasterror;
					errorcode = handler.lasterrorcode;
				}
				s.printf ("-ERR:%i:%S\n", errorcode, errorstr.cval());
				AUTHD->log (log::error, "worker", "Error %i: %S", errorcode,
							errorstr.cval());
			}
		}
	}
}

// ==========================================================================
// CONSTRUCTOR socketGroup
// ==========================================================================
socketGroup::socketGroup (void)
{
	shouldShutdown = false;
}

// ==========================================================================
// DESTRUCTOR socketGroup
// ==========================================================================
socketGroup::~socketGroup (void)
{
	//listenSock.o.close();
}

// ==========================================================================
// METHOD socketGroup::listenTo
// ==========================================================================
void socketGroup::listenTo (const string &inpath)
{
	exclusivesection (listenSock)
	{
		listenSock.listento (inpath);
	}
}

// ==========================================================================
// METHOD socketGroup::accept
// ==========================================================================
tcpsocket *socketGroup::accept (void)
{
	tcpsocket *res = NULL;
	if (shouldShutdown) return NULL;
	
	exclusivesection (listenSock)
	{
		if (! shouldShutdown) res = listenSock.tryaccept (2.5);
	}
	return res;
}

void socketGroup::shutdown (void)
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
// CONSTRUCTOR commandHandler
// ==========================================================================
commandHandler::commandHandler (void)
{
	transactionid = strutil::uuid ();
}

// ==========================================================================
// DESTRUCTOR commandHandler
// ==========================================================================
commandHandler::~commandHandler (void)
{
	if (module && transactionid)
	{
		value arg;
		arg[0] = transactionid;
		
		runScript ("end-transaction", arg);
	}
}

// ==========================================================================
// METHOD commandHandler::runScript
// ==========================================================================
bool commandHandler::runScriptExt (const string &scriptName,
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
// METHOD commandHandler::runScript
// ==========================================================================
bool commandHandler::runScript (const string &scriptName,
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

	AUTHD->log (log::info, "handler ", "Runscript module=<%S> id=<%S> "
				"name=<%S> argc=<%i>", module.str(),
				transactionid.str(), scriptName.str(), arguments.count());
	
	
	// Fill in the fully qualified path to the script.
	scriptPath.printf ("/var/opencore/tools/%s", scriptName.str());
	
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
// METHOD commandHandler::installFile
// ==========================================================================
bool commandHandler::installFile (const string &fname, const string &_dpath)
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
	
	AUTHD->log (log::info, "handler ", "Installfile module=<%S> id=<%S> "
				"name=<%S> dpath=<%S>",
				module.str(), transactionid.str(), fname.str(), dpath.str());
	
	tfname = guard.translateSource (module, fname, guarderr);
	if (! tfname)
	{
		AUTHD->log (log::info, "handler ", "Source policy fail: %s",
					guarderr.str());
		lasterrorcode = ERR_POLICY;
		lasterror = "Source file name does not match policy: ";
		lasterror.strcat (guarderr);
		return false;
	}
	
	if (! guard.checkDestination (module, fname, dpath, perms, guarderr))
	{
		AUTHD->log (log::info, "handler ", "Dest policy fail: %s",
					guarderr.str());
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
			AUTHD->log (log::error, "handler ", "Cannot find group %s",
						perms["group"].cval());
			lasterrorcode = ERR_NOT_FOUND;
			lasterror = "Unknown group: ";
			lasterror.strcat (perms["group"].sval());
			return false;
		}
	}
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
// METHOD commandHandler::makeDir
// ==========================================================================
bool commandHandler::makeDir (const string &_dpath)
{
	string tdname;
	value perms;
	string guarderr;
	string dpath = _dpath;
	
	if (dpath.strlen() && (dpath[-1] == '/'))
	{
		dpath.crop (dpath.strlen() - 1);
	}
	
	AUTHD->log (log::info, "handler ", "Makedir module=<%S> id=<%S> "
				"dpath=<%S>",
				module.str(), transactionid.str(), dpath.str());
	
	if (! guard.checkDestination (module, "", dpath, perms, guarderr))
	{
		AUTHD->log (log::info, "handler ", "Dest policy fail: %s",
					guarderr.str());
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
			AUTHD->log (log::error, "handler ", "Cannot find group %s",
						perms["group"].cval());
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
		AUTHD->log (log::warning, "handler", "Directory <%s> already "
					"existed when trying to create", dpath.str());
	}
	else if (!fs.mkdir (dpath))
	{
		AUTHD->log (log::error, "handler", "Cannot create dir <%s>",
					dpath.str());
		return false;
	}

	AUTHD->log (log::info, "handler", "Setting up perms: %s/%s %o",
				fuser.str(), fgroup.str(), mode);
	fs.chown (dpath, fuser, fgroup);
	fs.chmod (dpath, mode);
	
	return true;
}

bool commandHandler::makeUserDir (const string &dpath,
								  const string &user,
								  const string &modestr)
{
	string pdpath = (dpath[0] == '/') ? dpath.mid(1) : dpath;
	int mode = modestr.toint (8);
	if (dpath.strstr ("..") >= 0)
	{
		lasterrorcode = ERR_POLICY;
		lasterror = "Destination directory contains illegal characters";
		
		AUTHD->log (log::error, "handler", "Illegal characters in "
					"makeuserdir argument");
		return false;
	}
	
	string realpath;
	value pw;
	value ugr;
	value gr;
	uid_t destuid;
	gid_t destgid;
	
	gr = kernel.userdb.getgrnam ("paneluser");
	if (! gr)
	{
		lasterrorcode = ERR_NOT_FOUND;
		lasterror = "The paneluser group was not found";
		
		AUTHD->log (log::error, "handler", "No paneluser group found");
		return false;
	}
	
	pw = kernel.userdb.getpwnam (user);
	if (! pw)
	{
		lasterrorcode = ERR_NOT_FOUND;
		lasterror = "The user was not found";
		AUTHD->log (log::error, "handler", "Unknown user <%S>", user.str());
		return false;
	}
	
	destuid = pw["uid"].uval();
	destgid = pw["gid"].uval();
	
	ugr = kernel.userdb.getgrgid (destgid);
	if ( ! ugr)
	{
		lasterrorcode = ERR_NOT_FOUND;
		lasterror = "The user's primary group was not found";
		
		AUTHD->log (log::error, "handler", "Could not back-resolve gid #%u ",
					pw["gid"].uval());
		return false;
	}
	
	if (! gr["members"].exists (user))
	{
		lasterrorcode = ERR_POLICY;
		lasterror = "The user is not a member of group paneluser";
		
		AUTHD->log (log::error, "handler", "User <%S> not a member of "
					"group paneluser", user.str());
		gr.savexml ("/tmp/gr.xml");
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
			if (! fs.mkdir (tpath))
			{
				lasterrorcode = ERR_CMD_FAILED;
				lasterror = "Could not create directory";
				
				AUTHD->log (log::error, "handler", "Error creating "
							"directory <%S> for user <%S>"
							%format (tpath,user));
				return false;
			}
			
			int fd = open (tpath.str(), O_RDONLY);
			if (fd<0)
			{
				lasterrorcode = ERR_CMD_FAILED;
				lasterror = "Could open created directory";
				
				AUTHD->log (log::error, "handler", "Error open()ing "
							"created directory <%S> for user <%S>"
							%format (tpath,user));
			}
			
			if (fchown (fd,destuid,destgid))
			{
				close (fd);
				lasterrorcode = ERR_CMD_FAILED;
				lasterror = "Could not set ownership";
				
				AUTHD->log (log::error, "handler", "Error chowning "
							"directory <%S> for user <%S>",
							tpath.str(), user.str());
				return false;
			}
			
			if (fchmod (fd, mode))
			{
				close (fd);
				lasterrorcode = ERR_CMD_FAILED;
				lasterror = "Could not set permissions";
				
				AUTHD->log (log::error, "handler", "Error setting "
							"mode of directory <%S> for user <%S>",
							tpath.str(), user.str());
				return false;
			}
			
			close (fd);
		}
	}
	
	return true;
}

// ==========================================================================
// METHOD commandHandler::getObject
// ==========================================================================
bool commandHandler::getObject (const string &objname, file &out)
{
	string fname;
	string err;
	
	fname = guard.translateObject (module, objname, err);
	if (! fname)
	{
		AUTHD->log (log::error, "handler", "Cannot find object <%S>: %s",
					objname.str(), err.str());
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
// METHOD commandHandler::deleteDir
// ==========================================================================
bool commandHandler::deleteDir (const string &_dpath)
{
	string tdname;
	value perms;
	string guarderr;
	string dpath = _dpath;
	
	if (dpath.strlen() && (dpath[-1] == '/'))
	{
		dpath.crop (dpath.strlen() - 1);
	}
	
	AUTHD->log (log::info, "handler ", "Deletedir module=<%S> id=<%S> "
				"dpath=<%S>",
				module.str(), transactionid.str(), dpath.str());
	
	if (! guard.checkDestination (module, "", dpath, perms, guarderr))
	{
		AUTHD->log (log::info, "handler ", "Dest policy fail: %s",
					guarderr.str());
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
			AUTHD->log (log::error, "handler", "Directory <%S> does not match "
						"ownership policies", dpath.str());
			lasterrorcode = ERR_POLICY;
			lasterror = "Destination directory ownership mismatch";
			return false;
		}
	}
	if (perms.exists ("group"))
	{
		if (perms["group"] != inf["group"])
		{
			AUTHD->log (log::error, "handler", "Directory <%S> does not match "
						"ownership policies", dpath.str());
			lasterrorcode = ERR_POLICY;
			lasterror = "Destination directory ownership mismatch";
			return false;
		}
		value pw = kernel.userdb.getgrnam (perms["group"].sval());
	}
	
	value args;
	args.newval() = transactionid;
	args.newval() = dpath;
	
	return runScript ("remove-directory", args);
}

void commandHandler::finishTransaction (void)
{
	if (! transactionid) return;
	
	value args;
	args.newval() = transactionid;
	
	runScript ("end-transaction", args);
	
	AUTHD->log (log::info, "handler ", "Closing transaction module=<%S> "
			"id=<%S>", module.str(), transactionid.str());

	transactionid = nokey;
}

bool commandHandler::rollbackTransaction (void)
{
	if (! transactionid) return false;
	
	value args;
	args.newval() = transactionid;

	AUTHD->log (log::info, "handler ", "Rolling back transaction module=<%S> "
				"id=<%S>", module.str(), transactionid.str());

	return runScript ("rollback-transaction", args);
}

// ==========================================================================
// METHOD commandHandler::deleteFile
// ==========================================================================
bool commandHandler::deleteFile (const string &path)
{
	AUTHD->log (log::info, "handler ", "Delete file module=<%S> id=<%S> "
				"path=<%S>", module.str(), transactionid.str(), path.str());
	
	string guarderr;
	
	if (! guard.checkDelete (module, path, guarderr))
	{
		lasterrorcode = ERR_POLICY;
		lasterror     = "Destination file name does not match policy: ";
		lasterror.strcat (guarderr);
		return false;
	}
	
	value args;
	args.newval() = transactionid;
	args.newval() = path;
	
	return runScript ("remove-single-file", args);
}

// ==========================================================================
// METHOD commandHandler::createUser
// ==========================================================================
bool commandHandler::createUser (const string &userName, const string &ppass)
{
	static string validUser ("abcdefghijklmnopqrstuvwxyz0123456789_-."
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
	static string validPass ("abcdefghijklmnopqrstuvwxyz0123456789"
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()+="
							 "<>,./?;:'|{}[]-_`~ ");
	
	AUTHD->log (log::info, "handler ", "Create user module=<%S> id=<%S> "
				"name=<%S>", module.str(), transactionid.str(), userName.str());
	
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
	
	value args;
	args.newval() = transactionid;
	args.newval() = userName;
	args.newval() = ppass;
	
	return runScript ("create-system-user", args);
}

// ==========================================================================
// METHOD commandHandler::deleteUser
// ==========================================================================
bool commandHandler::deleteUser (const string &userName)
{
	static string validUser ("abcdefghijklmnopqrstuvwxyz0123456789_-."
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

	AUTHD->log (log::info, "handler ", "Delete user module=<%S> id=<%S> "
				"name=<%S>", module.str(), transactionid.str(), userName.str());
	
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
	
	value args;
	args.newval() = transactionid;
	args.newval() = userName;

	return runScript ("remove-system-user", args);
}

// ==========================================================================
// METHOD commandHandler::setUserShell
// ==========================================================================
bool commandHandler::setUserShell (	const string &userName,
							   		const string &shell)
{
	static string validUser ("abcdefghijklmnopqrstuvwxyz0123456789_-."
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

	AUTHD->log (log::info, "handler ", "Set User's Shell module=<%S> id=<%S> "
				"name=<%S>", module.str(), transactionid.str(), userName.str());
	
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
	
	value args;
	args.newval() = transactionid;
	args.newval() = userName;
	args.newval() = shell;

	return runScript ("change-system-usershell", args);
}

// ==========================================================================
// METHOD commandHandler::setUserShell
// ==========================================================================
bool commandHandler::setUserPass (	const string &userName,
							   		const string &password)
{
	static string validUser ("abcdefghijklmnopqrstuvwxyz0123456789_-."
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

	AUTHD->log (log::info, "handler ", "Set User's Shell module=<%S> id=<%S> "
				"name=<%S>", module.str(), transactionid.str(), userName.str());
	
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
	
	value args;
	args.newval() = transactionid;
	args.newval() = userName;
	args.newval() = password;

	return runScript ("change-user-password", args);
}

// ==========================================================================
// METHOD commandHandler::setQuota
// ==========================================================================
bool commandHandler::setQuota (const string &userName,
							   unsigned int softLimit,
							   unsigned int hardLimit)
{
	static string validUser ("abcdefghijklmnopqrstuvwxyz0123456789_-."
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

	AUTHD->log (log::info, "handler ", "Set User's quota module=<%S> id=<%S> user=<%S> "
				"soft/hard=<%d/%d>", module.str(), transactionid.str(), userName.str(),
				softLimit, hardLimit);
	
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
	
	value args;
	args.newval() = transactionid;
	args.newval() = userName;
	args.newval() = softLimit;
	args.newval() = hardLimit;

	return runScript ("change-user-quota", args);
}

// ==========================================================================
// METHOD pathguard::checkServiceAccess
// ==========================================================================
bool pathguard::checkServiceAccess (const string &moduleName,
									const string &serviceName,
									string &error)
{
	value meta;
	meta = cache.get (moduleName);
	if (! meta)
	{
		error = "Could not find module";
		AUTHD->log (log::error, "srvaccs", "Could not load module <%S>",
					moduleName.str());
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
// METHOD pathguard::checkScriptAccess
// ==========================================================================
bool pathguard::checkScriptAccess (const string &moduleName,
								   const string &scriptName,
								   string &userName,
								   string &error)
{
	value meta;
	
	AUTHD->log (log::info, "scraccs ", "Checking script access module=<%S> "
				"script=<%S>", moduleName.str(), scriptName.str());
				
	meta = cache.get (moduleName);
	if (! meta)
	{
		error = "Could not find module";
		AUTHD->log (log::error, "scraccs", "Could not load module <%S>",
					moduleName.str());
		return false;
	}
	
	if (! meta["authdops"]["scripts"].exists(scriptName))
	{
		AUTHD->log (log::error, "scraccs", "Script not defined in module.xml: <%S>",
					scriptName.str());
		error = "Script not defined in module.xml";
		return false;
	}
	
	value &scrip = meta["authdops"]["scripts"][scriptName];
	if (scrip.attribexists ("asroot"))
	{
		if ((scrip("asroot") == false) && (userName == "root"))
		{
			AUTHD->log (log::error, "scraccs", "Script <%S> may not be run as "
						"root as per the module.xml for <%S>",
						scriptName.str(), moduleName.str());
		}
	}
	
	if (scrip.attribexists ("asuser"))
	{
		userName = scrip("asuser");
	}

	AUTHD->log (log::info, "scraccs ", "Allowing script access module=<%S> "
				"script=<%S> user=<%S>", moduleName.str(), scriptName.str(),
				userName.str());
	
	return true;
}

// ==========================================================================
// METHOD pathguard::checkCommandAccess
// ==========================================================================
bool pathguard::checkCommandAccess (const string &moduleName,
								    const string &cmdName,
								    const string &cmdClass,
								    string &error)
{
	value meta;
	
	AUTHD->log (log::info, "cmdaccs ", "Checking command access module=<%S> "
				"command=<%S> commandclass=<%S>", moduleName.str(), cmdName.str(), cmdClass.str());
				
	meta = cache.get (moduleName);
	if (! meta)
	{
		error = "Could not find module";
		AUTHD->log (log::error, "cmdaccs", "Could not load module <%S>",
					moduleName.str());
		return false;
	}
	
	if (! meta["authdops"]["commands"].exists(cmdName)
	&&  ! meta["authdops"]["commandclasses"].exists(cmdClass))
	{
		AUTHD->log (log::error, "cmdaccs", "Command or command class not defined in module.xml");
		error = "Command or command class not defined in module.xml";
		return false;
	}
		
	AUTHD->log (log::info, "cmdaccs ", "Allowing command access module=<%S> ", moduleName.str());
	
	return true;
}

// ==========================================================================
// METHOD commandHandler::startService
// ==========================================================================
bool commandHandler::startService (const string &serviceName)
{
	AUTHD->log (log::info, "handler ", "Start service module=<%S> id=<%S> "
				"name=<%S>", module.str(), transactionid.str(),
				serviceName.str());
	
	value args;
	args.newval() = "start";
	args.newval() = serviceName;
	
	if (! guard.checkServiceAccess(module, serviceName, lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}

	return runScript ("control-service", args);
}

// ==========================================================================
// METHOD commandHandler::stopService
// ==========================================================================
bool commandHandler::stopService (const string &serviceName)
{
	AUTHD->log (log::info, "handler ", "Stop service module=<%S> id=<%S> "
				"name=<%S>", module.str(), transactionid.str(),
				serviceName.str());
	
	value args;
	args.newval() = "stop";
	args.newval() = serviceName;
		
	if (! guard.checkServiceAccess(module, serviceName, lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}

	return runScript ("control-service", args);
}

// ==========================================================================
// METHOD commandHandler::reloadService
// ==========================================================================
bool commandHandler::reloadService (const string &serviceName)
{
	AUTHD->log (log::info, "handler ", "Reload service module=<%S> id=<%S> "
				"name=<%S>", module.str(), transactionid.str(),
				serviceName.str());
	
	value args;
	args.newval() = "reload";
	args.newval() = serviceName;
		
	if (! guard.checkServiceAccess(module, serviceName, lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}

	return runScript ("control-service", args);
}

// ==========================================================================
// METHOD commandHandler::setServiceOnBoot
// ==========================================================================
bool commandHandler::setServiceOnBoot (const string &serviceName,
									   bool onBoot)
{
	AUTHD->log (log::info, "handler ", "Service onboot module=<%S> id=<%S> "
				"name=<%S> status=<%s>", module.str(), transactionid.str(),
				serviceName.str(), onBoot ? "on" : "off");
	
	value args;
	args.newval() = serviceName;
	args.newval() = onBoot ? 1 : 0;
	
	if (! guard.checkServiceAccess(module, serviceName, lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}
	
	return runScript ("control-service-boot", args);
}

// ==========================================================================
// METHOD commandHandler::setServiceOnBoot
// ==========================================================================
void commandHandler::setModule (const string &moduleName)
{
	module = moduleName;
	transactionid = strutil::uuid();
	
	AUTHD->log (log::info, "handler ", "Started transaction module=<%S> "
				"id=<%S>", module.str(), transactionid.str());
}

// ==========================================================================
// METHOD commandHandler::triggerSoftwareUpdate
// ==========================================================================
bool commandHandler::triggerSoftwareUpdate (void)
{
	tcpsocket s;
	
	if (! guard.checkCommandAccess(module, "osupdate", "", lasterror))
	{
		lasterrorcode = ERR_POLICY;
		return false;
	}
	
	
	if (! s.uconnect (PATH_SWUPD_SOCKET))
	{
		AUTHD->log (log::error, "handler", "Could not connect to swupd "
					"socket");
		return false;
	}
	
	try
	{
		s.writeln ("update");
		string line = s.gets();
		s.close ();
		if (line[0] == '+')
		{
			AUTHD->log (log::info, "handler", "Triggered software "
						"update");
			return true;
		}
	}
	catch (...)
	{
	}
	
	s.close ();
	AUTHD->log (log::error, "handler", "Error from swupd");
	return false;
}

// ==========================================================================
// CONSTRUCTOR metacache
// ==========================================================================
metacache::metacache (void)
{
}

// ==========================================================================
// DESTRUCTOR metacache
// ==========================================================================
metacache::~metacache (void)
{
}

// ==========================================================================
// METHOD metacache::get
// ==========================================================================
value *metacache::get (const statstring &moduleName)
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
	
	mxmlpath.printf ("/var/opencore/modules/%s.module/module.xml",
					 moduleName.str());

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
// CONSTRUCTOR pathguard
// ==========================================================================
pathguard::pathguard (void) : cache (MCache)
{
}

// ==========================================================================
// DESTRUCTOR pathguard
// ==========================================================================
pathguard::~pathguard (void)
{
}

// ==========================================================================
// METHOD pathguard::translateSource
// ==========================================================================
string *pathguard::translateSource (const statstring &moduleName,
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
		AUTHD->log (log::error, "pathgrd ", "Could not load module <%S>",
					moduleName.str());
		return NULL;
	}
	
	returnclass (string) res retain;
	
	foreach (op, meta["authdops"]["fileops"])
	{
		if (fileName.globcmp (op.id().sval()))
		{
			res.printf ("/var/opencore/conf/staging/%s/%s",
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
				if (finf["user"] != "opencore")
				{
					AUTHD->log (log::error, "pathgrd ", "Owner mismatch "
								"on file <%S>: %s", fileName.str(),
								finf["user"].cval());
					error = "File owner mismatch (not opencore)";
					res.crop();
				}
				else if (finf["group"] != "opencore")
				{
					AUTHD->log (log::error, "pathgrd ", "Group mismatch "
								"on file <%S>: %s", fileName.str(),
								finf["group"].cval());
					error = "File group mismatch (not opencore)";
					res.crop();
				}
				else
				{
					perms = finf["mode"].uval();
					if (perms & 1)
					{
						AUTHD->log (log::error, "pathgrd ", "Denied "
									"world-writable file <%S>",
									fileName.str());
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
// METHOD pathguard::checkDestination
// ==========================================================================
bool pathguard::checkDestination (const statstring &moduleName,
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
	AUTHD->log (log::warning, "pathgrd ", "Denied module=<%S> "
				"file=<%S> destpath=<%S>", moduleName.str(), sourceFile.str(),
				filePath.str());
	
	return false;
}

// ==========================================================================
// METHOD pathguard::translateDestination
// ==========================================================================
string *pathguard::translateDestination (const string &filePath,
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
// METHOD pathguard::translateObject
// ==========================================================================
string *pathguard::translateObject (const statstring &moduleName,
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
// METHOD pathguard::checkDelete
// ==========================================================================
bool pathguard::checkDelete (const statstring &moduleName,
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
	AUTHD->log (log::warning, "pathgrd ", "Denied delete module=<%S> "
				"file=<%S>", moduleName.str(), fullPath.str());
	
	return false;
}
