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

#ifndef _authd_H
#define _authd_H 1
#include <grace/daemon.h>
#include <grace/configdb.h>
#include <grace/thread.h>
#include <grace/tcpsocket.h>
#include <grace/lock.h>

#define ERR_INVALID_SCRIPT	4001
#define ERR_NOT_FOUND		4002
#define ERR_SCRIPT_FAILED	4003
#define ERR_POLICY			4004
#define ERR_NOT_IMPL		4005
#define ERR_CMD_FAILED		4006

//  -------------------------------------------------------------------------
/// A collection of worker threads that handle inbound connections.
//  -------------------------------------------------------------------------
class SocketGroup : public threadgroup
{
public:
						 /// Constructor.
						 SocketGroup (void);
						 
						 /// Destructor.
						~SocketGroup (void);
	
						 /// Set the listening socket.
	void				 listenTo (const string &path);
	
						 /// Shutdown all threads.
	void				 shutdown (void);
	
	tcpsocket			*accept (void);

protected:
	lock<tcplistener>	 listenSock;
	bool				 shouldShutdown;
};

//  -------------------------------------------------------------------------
/// Guardian for file operations. Uses the global MetaCache to
/// read module.xml meta-files and make sense of the fileops statements
/// therein.
//  -------------------------------------------------------------------------
class PathGuard
{
public:
						 /// Constructor.
						 /// \param c Pointer to the MetaCache.
						 PathGuard (void);
						 
						 /// Destructor.
						~PathGuard (void);
						
	string				*translateSource (const statstring &moduleName,
										  const string &fileName,
										  string &error);
										  
	bool				 checkDestination (const statstring &moduleName,
										   const string &sourceFile,
										   const string &filePath,
										   value &perms,
										   string &error);
	
	string				*translateObject (const statstring &moduleName,
										  const string &objname,
										  string &error);
	
	string				*translateDestination (const string &filePath,
										       const string &sourceFile);
	
	bool				 checkDelete (const statstring &moduleName,
									  const string &fullPath,
									  string &error);
									
	bool				 checkServiceAccess (const string &moduleName, 
											 const string &serviceName,
											 string &error);
											
	bool				 checkScriptAccess (const string &moduleName,
											const string &scriptName,
											string &userName,
											string &error);

	bool				 checkCommandAccess (const string &moduleName,
											 const string &cmdName,
											 const string &cmdClass,
											 string &error);
									
protected:
	class MetaCache		&cache;
};

//  -------------------------------------------------------------------------
/// A collection of handlers for command sent to the daemon.
//  -------------------------------------------------------------------------
class CommandHandler
{
public:
						 CommandHandler (void);
						~CommandHandler (void);
						
	void				 setModule (const string &moduleName);
						
						 /// Install a file to a user's homedir.
						 /// \param fileName The name of the file in the
						 ///                 source path to copy.
						 /// \param destPath The directory to copy to.
						 /// \param user The user we're messing with.
	bool				 installUserFile (const string &fileName,
									  	 const string &destPath,
										 const string &user);

						 /// Install a file to the filesystem.
						 /// \param fileName The name of the file in the
						 ///                 source path to copy.
						 /// \param destPath The directory to copy to.
	bool				 installFile (const string &fileName,
									  const string &destPath,
									  uid_t uid=0,
									  gid_t gid=0);
						 
						 /// Remove a file from the filesystem.
						 /// \param fileName The name of the file to
						 ///                 delete.
						 /// \param filePath The path of the file.
	bool				 deleteFile (const string &filePath);

						 /// Create a directory.
						 /// \param dirPath The directory to create.
	bool				 makeDir (const string &dirPath);
	
	bool				 makeUserDir (const string &dpath,
									  const string &user,
									  const string &modestr);
									  
						 /// Delete a directory
						 /// \param dirPath The directory to remove.
	bool				 deleteDir (const string &dirPath);

						 /// Create a user account.
						 /// \param userName the user's username
						 /// \param plainPass The plaintext password.
	bool				 createUser (const string &userName,
									 const string &plainPass);
	
						 /// Delete a user account.
						 /// \param userName the user's username.
	bool				 deleteUser (const string &userName);
	
						 /// Set a user's quota.
						 /// \param userName the user's username.
						 /// \param softLimit the quota soft limit.
						 /// \param hardLimit the quota hard limit.
	bool				 setQuota (const string &userName,
								   unsigned int softLimit,
								   unsigned int hardLimit);
	
	
	bool 				 setUserShell (	const string &userName,
							   			const string &shell);

	bool 				 setUserPass (	const string &userName,
							   			const string &password);

	
						 /// Start a system service.
						 /// \param serviceName The SysV service name.
	bool				 startService (const string &serviceName);
	
						 /// Stop a system service.
						 /// \param serviceName The SysV service name.
	bool				 stopService (const string &serviceName);
	
						 /// Reload a system service configuration.
						 /// \param serviceName The SysV service name.
	bool				 reloadService (const string &serviceName);
	
						 /// Saves current service state
						 /// \param serviceName The SysV service name.
	bool				 saveService (const string &serviceName);
	
						 /// Restores last saved service state
						 /// \param serviceName The SysV service name.
	bool				 restoreService (const string &serviceName);
	
	
						 /// Set the onboot status of a system service.
						 /// \param servicename The SysV service name.
						 /// \param onBoot The start-up flag.
	bool				 setServiceOnBoot (const string &serviceName,
										   bool onBoot);
	
	bool				 getObject (const string &, file &);
	
						 /// Run a specific script from the allowed
						 /// scripts directory.
	bool				 runScript (const string &scriptName,
									const value &arguments,
									const string &user = "root");

						 /// Run a specific script from the allowed
						 /// scripts directory based on a direct external
						 /// request.
	bool				 runScriptExt (const string &scriptName,
									   const value &arguments,
									   const string &user = "root");
	
						 /// Send an update-trigger to the swupd process.
	bool				 triggerSoftwareUpdate (void);
	
	void				 finishTransaction (void);
	
	bool				 rollbackTransaction (void);
	
	string				 lasterror; ///< Last generated error text.
	int					 lasterrorcode; ///< Last generated error code.
	string				 transactionid; ///< The transaction-id.
	statstring			 module; ///< Associated module name.
	
protected:
	class PathGuard		 guard; ///< Our personal psychologist.
};

//  -------------------------------------------------------------------------
/// A cache for meta-information that comes with a module.
/// This class acts as a loader for the module.xml files that come with
/// modules, but will cache each result up to 60 seconds. This is
/// expected to work better than building a static list at start-up time,
/// because it will allow the daemon to keep on running despite changes
/// to a module's meta-data or the installation of a new module.
//  -------------------------------------------------------------------------
class MetaCache
{
public:
						 MetaCache (void);
						~MetaCache (void);
						
						 /// Get a specific module's metadata.
	value				*get (const statstring &moduleName);

protected:
	lock<value>			 cache; ///< cached metabase.
};

extern MetaCache MCache;

//  -------------------------------------------------------------------------
/// A worker thread for handling a connection to the daemon.
//  -------------------------------------------------------------------------
class SocketWorker : public groupthread
{
public:
							 /// Constructor.
							 /// \param grp The parent group.
							 SocketWorker (class SocketGroup *grp);
						 
							 /// Destructor.
							~SocketWorker (void);
			
							 /// Run-method, performs the thread's job.
	void					 run (void);
	void					 handle (tcpsocket &s);

protected:
	class SocketGroup		*group; ///< The parent group.
	class CommandHandler	 handler; ///< The command handler.
	bool					 shouldShutdown;
};

//  -------------------------------------------------------------------------
/// Implementation template for application config.
//  -------------------------------------------------------------------------
typedef configdb<class AuthdApp> appconfig;

//  -------------------------------------------------------------------------
/// Main daemon class.
//  -------------------------------------------------------------------------
class AuthdApp : public daemon
{
public:
		 				 AuthdApp (void);
		 				~AuthdApp (void);
		 	
	int					 main (void);
	
	bool				 shouldRun;
	
protected:
	bool				 confLog (config::action act, keypath &path,
								  const value &nval, const value &oval);

	appconfig			 conf;
};

#endif
