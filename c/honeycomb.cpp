/*---------------------------- Includes ------------------------------------*/
#include <string>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <assert.h>
#include <fcntl.h>
#include <regex.h>
#include <libgen.h>
// System
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>

// Include sys/capability if the system can handle it. 
#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif
// Erlang interface
// #include "ei++.h"

#include "honeycomb_config.h"
#include "honeycomb.h"
#include "hc_support.h"
#include "babysitter_utils.h"

/*---------------------------- Implementation ------------------------------*/

// using namespace ei;
#define DEFAULT_PATH "/bin:/usr/bin:/usr/local/bin:/sbin;"

int Honeycomb::setup_defaults() {
  return 0;
}
int Honeycomb::build_env_vars() {
  /* Setup environment defaults */
  char temp[BUF_SIZE]; memset(temp, 0, BUF_SIZE);
  char user_id_buf[BUF_SIZE]; memset(user_id_buf, 0, BUF_SIZE); sprintf(user_id_buf, "APP_USER=%o", user());
  char app_type_buf[BUF_SIZE]; memset(app_type_buf, 0, BUF_SIZE); sprintf(app_type_buf, "APP_TYPE=%s", app_type());
  char group_id_buf[BUF_SIZE]; memset(group_id_buf, 0, BUF_SIZE); sprintf(group_id_buf, "APP_GROUP=%o", group());
  char image_buf[BUF_SIZE]; memset(image_buf, 0, BUF_SIZE); sprintf(image_buf, "BEE_IMAGE=%s", image());
  char sha_buf[BUF_SIZE]; memset(sha_buf, 0, BUF_SIZE); sprintf(sha_buf, "BEE_SHA=%s", sha());
  char scm_url_buf[BUF_SIZE]; memset(scm_url_buf, 0, BUF_SIZE); sprintf(scm_url_buf, "SCM_URL=%s", scm_url());
  char m_size_buf[BUF_SIZE]; memset(m_size_buf, 0, BUF_SIZE); sprintf(m_size_buf, "BEE_SIZE=%d", (int)(m_size / 1024));
  char app_root_buf[BUF_SIZE]; memset(app_root_buf, 0, BUF_SIZE); sprintf(app_root_buf, "WORKING_DIR=%s", working_dir());
  char app_name_buf[BUF_SIZE]; memset(app_name_buf, 0, BUF_SIZE); sprintf(app_name_buf, "APP_NAME=%s", name());
  char storage_dir_buf[BUF_SIZE]; memset(storage_dir_buf, 0, BUF_SIZE); sprintf(storage_dir_buf, "STORAGE_DIR=%s", storage_dir());
  char bee_port_buf[BUF_SIZE]; memset(bee_port_buf, 0, BUF_SIZE); sprintf(bee_port_buf, "BEE_PORT=%d", port());
  char run_dir_buf[BUF_SIZE]; memset(run_dir_buf, 0, BUF_SIZE); sprintf(run_dir_buf, "RUN_DIR=%s", run_dir());
  
  // BEE_WORKING_DIR
  std::string pth = DEFAULT_PATH;
  unsigned int path_size = pth.length();
  char path_buf[path_size]; 
  memset(path_buf, 0, path_size); 
  sprintf(path_buf, "PATH=%s", pth.c_str());
  
  const char* default_env_vars[] = {
   app_name_buf,
   storage_dir_buf,
   app_root_buf,
   user_id_buf,
   group_id_buf,
   run_dir_buf,
   bee_port_buf,
   image_buf,
   sha_buf,
   scm_url_buf,
   m_size_buf,
   path_buf,
   app_type_buf,
   "LD_LIBRARY_PATH=/lib;/usr/lib;/usr/local/lib", 
   "HOME=/home/app",
   "FILESYSTEM=ext3",
   NULL
  };
  
  m_cenv_c = sizeof(default_env_vars) / sizeof(char *);
    
  if ( (m_cenv = (const char **) malloc(sizeof(char *) * m_cenv_c)) == NULL ) {
    fprintf(stderr, "Could not allocate a new char. Out of memory\n");
    exit(-1);
  }
  
  memset(m_cenv, 0, (int)sizeof(default_env_vars));
  memcpy(m_cenv, default_env_vars, (int)sizeof(default_env_vars));
  
  return 0;
}

#ifndef MAX_ARGS
#define MAX_ARGS 64
#endif
// Run a hook on the system
int Honeycomb::comb_exec(std::string cmd, bool should_wait = true) {
  setup_defaults(); // Setup default environments
  build_env_vars();
  
  const std::string shell = getenv("SHELL");  
  const std::string shell_args = "-c";
  const char* argv[] = { shell.c_str(), shell_args.c_str(), cmd.c_str(), NULL };
  pid_t pid; // Pid of the child process
  int status;
  
  // If we are looking at shell script text
  if ( strncmp(cmd.c_str(), "#!", 2) == 0 ) {
    char filename[40];
    int size, fd;
    
    snprintf(filename, 40, "/tmp/babysitter.XXXXXXXXX");
    
    // Make a tempfile in the filename format
    if ((fd = mkstemp(filename)) == -1) {
      fprintf(stderr, "Could not open tempfile: %s\n", filename);
      return -1;
    }
    
    size = strlen(cmd.c_str());
    // Print the command into the file
    if (write(fd, cmd.c_str(), size) == -1) {
      fprintf(stderr, "Could not write command to tempfile: %s\n", filename);
      return -1;
    }
    
    // Confirm that the command is written
    if (fsync(fd) == -1) {
      fprintf(stderr, "fsync failed for tempfile: %s\n", filename);
      return -1;
    }
    
    close(fd);
    
    // Modify the command to match call the filename
    std::string sFile (filename);
    
		if (chown(sFile.c_str(), m_user, m_group) != 0) {
#ifdef DEBUG
     fprintf(stderr, "Could not change owner of '%s' to %d\n", sFile.c_str(), m_user);
#endif
		}

    // Make it executable
    if (chmod(sFile.c_str(), 040700) != 0) {
      fprintf(stderr, "Could not change permissions to '%s' %o\n", sFile.c_str(), 040750);
    }
    
    // Run in a new process
    if ((pid = fork()) == -1) {
      perror("fork");
      return -1;
    }
    if (pid == 0) {
      // we are in a new process
      argv[2] = sFile.c_str();
      argv[3] = NULL;
      // int i = 0;
      //for (i = 0; i < 4; i++) printf("argv[%i] = %s\n", i, argv[i]);
      
      if (execve(argv[2], (char* const*)argv, (char* const*)m_cenv) < 0) {
        fprintf(stderr, "Cannot execute file '%s' because '%s'", argv[2], ::strerror(errno));
        perror("execute");
        unlink(filename);
        return -1;
      }
    }
    if (should_wait) {
      while (wait(&status) != pid) ; // We don't want to continue until the child process has ended
      assert(0 == status);
      
      // Cleanup :)
      unlink(filename);
    } 
    // Cleanup :)
  } else {
    
    // First, we have to construct the command
    int argc = 0;
    char str_cmd[BUF_SIZE];
    memset(str_cmd, 0, BUF_SIZE); // Clear it
    
    memcpy(str_cmd, cmd.c_str(), strlen(cmd.c_str()));
    
    argv[argc] = strtok(str_cmd, " \r\t\n");
    // Remove the first one
    std::string bin = find_binary(argv[argc]);
    argv[argc] = bin.c_str();
    
    // argv[argc] = strtok(str_cmd, " \r\t\n");
    
    while (argc++ < MAX_ARGS) if (! (argv[argc] = strtok(NULL, " \t\n")) ) break;
    
    // Run in a new process
    if ((pid = fork()) == -1) {
      perror("fork");
      return -1;
    }
    if (pid == 0) {
      // we are in the child process
      
      if (execve(argv[0], (char* const*)argv, (char* const*)m_cenv) < 0) {
        fprintf(stderr, "Cannot execute '%s' because '%s'\n", argv[0], ::strerror(errno));
        return EXIT_FAILURE;
      }
    }
    
    if (should_wait) {
      while (wait(&status) != pid) ; // We don't want to continue until the child process has ended
      assert(0 == status);
    }
  }
  
  return 0;
}

// Execute a hook
void Honeycomb::exec_hook(std::string action, int stage, phase *p) {
  if (stage == BEFORE) {
    if (p->before) comb_exec(p->before); //printf("Run before hook for %s: %s\n", action.c_str(), p->before);
  } else if (stage == AFTER) {
    if (p->after) comb_exec(p->after); //printf("Run after hook for %s %s\n", action.c_str(), p->after);
  } else {
    printf("Unknown hook for: %s %d\n", action.c_str(), stage);
  }
}

void Honeycomb::ensure_exists(std::string dir) {
  mkdir_p(dir, m_user, m_group, m_mode);
}

string_set *Honeycomb::string_set_from_lines_in_file(std::string filepath) {
  string_set *lines = new string_set();
  FILE *fp;
  char line[BUF_SIZE];
  int len;
  
  // If we weren't given a file
  if (filepath == "") {
    fprintf(stderr, "string_set_from_lines_in_file:");
    return lines;
  }
  
  // Open the file
  if ((fp = fopen(filepath.c_str(), "r")) == NULL) {
    fprintf(stderr, "Could not open '%s'\n", filepath.c_str());
    return lines;
  }
    
  memset(line, 0, BUF_SIZE);
  
  while ( fgets(line, BUF_SIZE, fp) != NULL) {
    len = (int)strlen(line) - 1;
    
    // Chomp the beginning of the line
    for(int i = 0; i < len; ++i) {
      if ( isspace(line[i]) ) {        
        for (int j = 0; j < len; ++j) {
          line[j] = line[j+1];
        }
        line[len--] = 0;
      } else {
        break;
      }
    }
    // Chomp the end of the line
    while ((len>=0) && (isspace(line[len])) ) {
      line[len] = 0;
      len--;
    }
    // Skip newlines
    if (line[0] == '\n') continue;
    // Insert 
    lines->insert(line);
  }
  
  return lines;
}

void Honeycomb::setup_internals()
{
  // The m_working_dir path is the confinement_root plus the user's uid
  // because it's randomly generated
  std::string usr_p (to_string(m_user, 10));
  std::string usr_postfix = m_name + "/" + usr_p;
  
  m_working_dir = m_working_dir + "/" + usr_postfix;
  m_run_dir = m_run_dir +  "/" + usr_postfix;
  // m_storage_dir = m_storage_dir + "/" + usr_postfix;
}

//---
// ACTIONS
//---

int Honeycomb::bundle(int dlvl) {
  setup_internals();
  debug(dlvl, 3, "Finding the bundle phase in our config (%p)\n", m_honeycomb_config);
  phase *p = find_phase(m_honeycomb_config, T_BUNDLE);
  
  debug(dlvl, 3, "Found the phase for the bundling action: %p\n", p);
  
  debug(dlvl, 3, "Running before hook for bundling\n");
  
  exec_hook("bundle", BEFORE, p);
  // Run command
  //--- Make sure the directory exists
  debug(dlvl, 3, "Making sure the working directory: '%s' exists\n", m_working_dir.c_str());
  ensure_exists(m_working_dir);
  
  temp_drop();
  
  // Change into the working directory
  if (chdir(m_working_dir.c_str())) {
    perror("chdir:");
    return -1;
  }
  
  // Clone the app in the directory if there is an scm_url
  // Get the clone command
  if (m_scm_url != "") {
    DEBUG_MSG("Cloning from %s\n", m_scm_url.c_str());
    std::string clone_command ("git clone --depth 0 %s home/app");
    if (m_honeycomb_config->clone_command != NULL) clone_command = m_honeycomb_config->clone_command;
    char str_clone_command[256];
    // The str_clone_command looks like:
    //  /usr/bin/git clone --depth 0 [git_url] [m_working_dir]/home/app
    snprintf(str_clone_command, 256, clone_command.c_str(), m_scm_url.c_str());
    // Do the cloning
    // Do not like me some system(3)
    int status, argc = 0;
    const char* argv[64];
    pid_t pid;
    // Get the binary
    std::string bin = find_binary("git");
    argv[argc] = bin.c_str();
    // Remove the first one
    strtok(str_clone_command, " \r\t\n");

    while (argc++ < MAX_ARGS) if (! (argv[argc] = strtok(NULL, " \t\n")) ) break;

    // Run in a new process
    if ((pid = fork()) == -1) {
      perror("fork");
      return -1;
    }
    if (pid == 0) {
      // we are in the child process
      if (execve(argv[0], (char* const*)argv, (char* const*)m_cenv) < 0) {
        fprintf(stderr, "Cannot execute '%s' because '%s'\n", argv[0], ::strerror(errno));
        return EXIT_FAILURE;
      }
    }

    while (wait(&status) != pid) ; // We don't want to continue until the child process has ended
    assert(0 == status);
    
    std::string git_root_dir = m_working_dir + "/home/app";
    m_sha = parse_sha_from_git_directory(git_root_dir);
    m_size = dir_size_r(m_working_dir.c_str());
  }
  
  if ((p != NULL) && (p->command != NULL)) {
    debug(dlvl, 1, "Running client code: %s\n", p->command);
    // Run the user's command
    comb_exec(p->command);
  } else {
    //Default action
    printf("Running default action for bundle\n");
  }
  restore_perms();
  
  exec_hook("bundle", AFTER, p);
  
  debug(dlvl, 2, "Removing working directory: %s\n", working_dir());
  rmdir_p(m_working_dir);
  
  return 0;
}

int Honeycomb::start(int dlvl) 
{
  setup_internals();
  debug(dlvl, 3, "Finding the bundle phase in our config (%p)\n", m_honeycomb_config);
  phase *p = find_phase(m_honeycomb_config, T_START);
  
  debug(dlvl, 3, "Found the phase for the bundling action: %p\n", p);
  
  debug(dlvl, 3, "Running before hook for bundling\n");
  
  exec_hook("start", BEFORE, p);
  // Run command
  //--- Make sure the directory exists
  ensure_exists(m_run_dir);
  
  temp_drop();
  
  // Change into the working directory
  if (chdir(m_run_dir.c_str())) {
    perror("chdir:");
    return -1;
  }
  
  if ((p != NULL) && (p->command != NULL)) {
    debug(dlvl, 1, "Running client code: %s\n", p->command);
    // Run the user's command
    comb_exec(p->command, false);
  } else {
    //Default action
    printf("Running default action for bundle\n");
  }
  
  restore_perms();
  
  exec_hook("start", AFTER, p);
  return 0;
}
int Honeycomb::stop(int dlvl)
{
  return 0;
}
int Honeycomb::mount(int dlvl)
{
  return 0;
}
int Honeycomb::unmount(int dlvl)
{
  return 0;
}
int Honeycomb::cleanup(int dlvl)
{
  return 0;
}
void Honeycomb::set_rlimits() {
  if(m_nofiles) set_rlimit(RLIMIT_NOFILE, m_nofiles);
}

int Honeycomb::set_rlimit(const int res, const rlim_t limit) {
  struct rlimit lmt = { limit, limit };
  if (setrlimit(res, &lmt)) {
    fprintf(stderr, "Could not set resource limit: %d\n", res);
    return(-1);
  }
  return 0;
}

/*---------------------------- UTILS ------------------------------------*/

const char *DEV_RANDOM = "/dev/urandom";
uid_t Honeycomb::random_uid() {
  uid_t u;
  for (unsigned char i = 0; i < 10; i++) {
    int rndm = open(DEV_RANDOM, O_RDONLY);
    if (sizeof(u) != read(rndm, reinterpret_cast<char *>(&u), sizeof(u))) {
      continue;
    }
    close(rndm);

    if (u > 0xFFFF) {
      return u;
    }
  }

  DEBUG_MSG("Could not generate a good random UID after 10 attempts. Bummer!");
  return(-1);
}

std::string Honeycomb::find_binary(const std::string& file) {
  // assert(geteuid() != 0 && getegid() != 0); // We can't be root to run this.
  
  if (file == "") return "";
  
  if (abs_path(file)) return file;
  
  std::string pth = DEFAULT_PATH;
  char *p2 = getenv("PATH");
  if (p2) {pth = p2;}
  
  std::string::size_type i = 0;
  std::string::size_type f = pth.find(":", i);
  do {
    std::string s = pth.substr(i, f - i) + "/" + file;
    
    if (0 == access(s.c_str(), X_OK)) {return s;}
    i = f + 1;
    f = pth.find(':', i);
  } while(std::string::npos != f);
  
  if (!abs_path(file)) {
    fprintf(stderr, "Could not find the executable %s in the $PATH\n", file.c_str());
    return NULL;
  }
  if (0 == access(file.c_str(), X_OK)) return file;
  
  fprintf(stderr, "Could not find the executable %s in the $PATH\n", file.c_str());
  return NULL;
}

bool Honeycomb::abs_path(const std::string & path) {
  return FS_SLASH == path[0] || ('.' == path[0] && FS_SLASH == path[1]);
}

// Turn to string, by base
const char * const Honeycomb::to_string(long long int n, unsigned char base) {
  static char bfr[32];
  const char * frmt = "%lld";
  if (16 == base) {frmt = "%llx";} else if (8 == base) {frmt = "%llo";}
  snprintf(bfr, sizeof(bfr) / sizeof(bfr[0]), frmt, n);
  return bfr;
}

/*---------------------------- Permissions ------------------------------------*/

int Honeycomb::temp_drop() {

#ifdef HAS_SETRESGID
  DEBUG_MSG("Dropping into '%d' user\n", (int)m_user);
  if (setresgid(-1, m_user, getegid()) || setresuid(-1, m_user, geteuid()) || getegid() != m_user || geteuid() != m_user ) {
    fprintf(stderr, "Could not drop privileges temporarily to %d: %s\n", m_user, ::strerror(errno));
    return -1; // we are in the fork
  }
#ifdef DEBUG
  printf("Dropped into user %d\n", m_user);
#endif
  return 0;
#else
  return 1;
#endif
}
int Honeycomb::perm_drop() {
#ifdef HAS_SETRESGID
  uid_t ruid, euid, suid;
  gid_t rgid, egid, sgid;
  if (setresgid(m_user, m_user, m_user) || setresuid(m_user, m_user, m_user) || getresgid(&rgid, &egid, &sgid)
      || getresuid(&ruid, &euid, &suid) || rgid != m_user || egid != m_user || sgid != m_user
      || ruid != m_user || euid != m_user || suid != m_user || getegid() != m_user || geteuid() != m_user ) {
    fprintf(stderr,"\nError setting user to %u\n",m_user);
    return -1;
  }
  return 0;
#else
  return 1;
#endif
}
int Honeycomb::restore_perms() {
#ifdef HAS_SETRESGID
  uid_t ruid, euid, suid;
  gid_t rgid, egid, sgid;
  DEBUG_MSG("Recovering into '%d' user\n", (int)geteuid());

  if (getresgid(&rgid, &egid, &sgid) || getresuid(&ruid, &euid, &suid) || setresuid(-1, suid, -1) || setresgid(-1, sgid, -1)
      || geteuid() != suid || getegid() != sgid ) {
        fprintf(stderr, "Could not restore privileges to %d: %s\n", m_user, ::strerror(errno));
        return -1; // we are in the fork
      }
  return 0;
#else
  return 1;
#endif
}

/*------------------------ INTERNAL -------------------------*/
void Honeycomb::init() {
  // ei::Serializer m_eis(2);
  m_nofiles = NULL;
  std::stringstream stream;
  std::string token;
  
  // Setup defaults here
  m_user = INT_MAX;
  m_group = INT_MAX;
  
  // Run through the config
  if (m_honeycomb_config != NULL) {
    //--- User
    if (m_honeycomb_config->user != NULL) {
      struct passwd *pw = getpwnam(m_honeycomb_config->user);
      if (pw == NULL) {
        fprintf(stderr, "Error: invalid user %s : %s\n", m_honeycomb_config->user, ::strerror(errno));
        m_user = geteuid();
      } else m_user = pw->pw_uid;
    } else m_user = random_uid();
    
    //--- group
    if (m_honeycomb_config->group != NULL) {
      struct group *grp = getgrnam(m_honeycomb_config->group);
      if (grp == NULL) {
        fprintf(stderr, "Error: invalid group %s : %s\n", m_honeycomb_config->group, ::strerror(errno));
        m_group = getgid();
      }
      else m_group = grp->gr_gid;
    } else m_group = random_uid();
    
    //--- root_directory
    if (m_honeycomb_config->root_dir != NULL) m_root_dir = m_honeycomb_config->root_dir;
    //--- storage_dir
    if (m_honeycomb_config->storage_dir != NULL) m_storage_dir = m_honeycomb_config->storage_dir;
    //--- run_dir
    if (m_honeycomb_config->run_dir != NULL) m_run_dir = m_honeycomb_config->run_dir;
    
    //--- executables
    m_executables.insert("/bin/ls");
    m_executables.insert("/bin/bash");
    m_executables.insert("/usr/bin/whoami");
    m_executables.insert("/usr/bin/env");
    m_executables.insert(getenv("SHELL"));
    
    if (m_honeycomb_config->executables != NULL) {
      stream << m_honeycomb_config->executables; // Insert into a string
      while ( getline(stream, token, ' ') ) m_executables.insert(token);
    }
    
    //--- directories
    if (m_honeycomb_config->directories != NULL) {
      stream.clear(); token = "";
      stream << m_honeycomb_config->directories;
      while ( getline(stream, token, ' ') ) m_dirs.insert(token);
    }
    
    //--- other files
    if (m_honeycomb_config->files != NULL) {
      stream.clear(); token = "";
      stream << m_honeycomb_config->files;
      while ( getline(stream, token, ' ') ) m_files.insert(token);
    }
    
    //--- run directory
    if (m_honeycomb_config->run_dir != NULL) m_run_dir = m_honeycomb_config->run_dir;
    
    //--- image
    if (m_honeycomb_config->image != NULL) m_image = m_honeycomb_config->image;
    //--- skel directory
    if (m_honeycomb_config->skel_dir != NULL) m_skel_dir = m_honeycomb_config->skel_dir;    
    
    //--- confinement_mode
    m_mode = S_IRWXU | S_IRGRP | S_IROTH; // Not sure if this should be dynamic-able, yet.
    
    //--- We need a name!
    if (m_name == "") m_name = to_string(random_uid(), 10);
  }
}

int Honeycomb::valid() {
  // Run validations on the honeycomb here
  return 0;
}