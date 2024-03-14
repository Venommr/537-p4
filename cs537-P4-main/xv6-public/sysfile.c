//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "wmap.h"
#include "memlayout.h"


// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *curproc = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd] == 0){
      curproc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int
sys_read(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int
sys_write(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

int
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int
sys_fstat(void)
{
  struct file *f;
  struct stat *st;

  if(argfd(0, 0, &f) < 0 || argptr(1, (void*)&st, sizeof(*st)) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
int
sys_link(void)
{
  char name[DIRSIZ], *new, *old;
  struct inode *dp, *ip;

  if(argstr(0, &old) < 0 || argstr(1, &new) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

//PAGEBREAK!
int
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], *path;
  uint off;

  if(argstr(0, &path) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

int
sys_open(void)
{
  char *path;
  int fd, omode;
  struct file *f;
  struct inode *ip;

  if(argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  end_op();

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

int
sys_mkdir(void)
{
  char *path;
  struct inode *ip;

  begin_op();
  if(argstr(0, &path) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_mknod(void)
{
  struct inode *ip;
  char *path;
  int major, minor;

  begin_op();
  if((argstr(0, &path)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEV, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_chdir(void)
{
  char *path;
  struct inode *ip;
  struct proc *curproc = myproc();
  
  begin_op();
  if(argstr(0, &path) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(curproc->cwd);
  end_op();
  curproc->cwd = ip;
  return 0;
}

int
sys_exec(void)
{
  char *path, *argv[MAXARG];
  int i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    if(fetchint(uargv+4*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(uarg, &argv[i]) < 0)
      return -1;
  }
  return exec(path, argv);
}

int
sys_pipe(void)
{
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if(argptr(0, (void*)&fd, 2*sizeof(fd[0])) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      myproc()->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}

uint 
sys_wmap(void){
    uint addr;
    int length;
    int flags;
    int fd;
    struct file* f = (void*) 0;
	
    if(argint(1, &length) < 0 || argint(2, &flags) < 0){
      	return FAILED;
    }
	if(argint(0, (int*) &addr)){
		return FAILED;
	}
	if (argfd(3, &fd, &f) < 0 && !(flags & MAP_ANONYMOUS)) {
			return FAILED;
  	}
	if (!(flags & MAP_PRIVATE) && !(flags & MAP_SHARED)) {
		// Invalid arguements
		return FAILED;
	}
	
	// When size provided is less or equal to zero and offset is less than zero
	if (length <= 0) {
		return FAILED;
	}

	struct proc* p = myproc();
	int total_mmaps = p->wminfo->wmapinfo->total_mmaps;
	if(total_mmaps == MAX_WMMAP_INFO) return FAILED;

	int pages = PGROUNDUP(length)/PGSIZE;
	if(p->pginfo->n_upages + pages > MAX_UPAGE_INFO) return FAILED;
	int found = -1;

	if(flags & MAP_FIXED){
		uint maddr = PGROUNDUP(addr);
		
		if(total_mmaps == 0 || PGROUNDUP(p->wminfo->wmapinfo->addr[total_mmaps - 1] + p->wminfo->wmapinfo->addr[total_mmaps - 1]) < maddr){
			p->wminfo->wmapinfo->addr[total_mmaps] = addr;
			p->wminfo->wmapinfo->length[total_mmaps] = length;
			found = 0;
		}
		for(int i = 0; i < p->wminfo->wmapinfo->total_mmaps - 1; i++){
			//check the free space i.e (addr[i] + length => addr[i+1])
			if(maddr > p->wminfo->wmapinfo->addr[i] + p->wminfo->wmapinfo->length[i] 
				&& maddr < p->wminfo->wmapinfo->addr[i+1] 
				&& p->wminfo->wmapinfo->addr[i+1] - p->wminfo->wmapinfo->addr[i] + p->wminfo->wmapinfo->length[i]){
					//if free space large enough found shift array and observe mappings
					for(int j = total_mmaps; j > i; j--){
						p->wminfo->wmapinfo->addr[j] = p->wminfo->wmapinfo->addr[j-1];
						p->wminfo->wmapinfo->n_loaded_pages[j] = p->wminfo->wmapinfo->n_loaded_pages[j-1];
						p->wminfo->wmapinfo->length[j] = p->wminfo->wmapinfo->length[j-1];
						p->wminfo->flags[j] = p->wminfo->flags[j-1];
						p->wminfo->fds[j] = p->wminfo->fds[j-1];
					}
					p->wminfo->wmapinfo->addr[total_mmaps] = addr;
					p->wminfo->wmapinfo->length[total_mmaps] = length;
					found = i;
			}
		}
	}  else {
		//find best fit unmapped space
		int bestsize = KERNBASE;
		int freespace;
		if(total_mmaps == 0){
			p->wminfo->wmapinfo->addr[0] = 0;
			p->wminfo->wmapinfo->length[0] = length;
			found = 0;
		} 
		for(int i = 0; i < total_mmaps - 1; i++){
			freespace = p->wminfo->wmapinfo->addr[i+1] - p->wminfo->wmapinfo->addr[i] + p->wminfo->wmapinfo->length[i];
			if(length < freespace && freespace < bestsize){
				found = i;
				bestsize = freespace;
			}
		}
		freespace = KERNBASE - p->wminfo->wmapinfo->addr[total_mmaps - 1] + p->wminfo->wmapinfo->length[total_mmaps - 1];
		if(length < freespace && freespace < bestsize){
			found = total_mmaps - 1;
			bestsize = freespace;
		}

		if(found == -1) return FAILED;
		for(int i = total_mmaps; i > found; i--){
			p->wminfo->wmapinfo->addr[i] = p->wminfo->wmapinfo->addr[i-1];
			p->wminfo->wmapinfo->n_loaded_pages[i] = p->wminfo->wmapinfo->n_loaded_pages[i-1];
			p->wminfo->wmapinfo->length[i] = p->wminfo->wmapinfo->length[i-1];
			p->wminfo->flags[i] = p->wminfo->flags[i-1];
			p->wminfo->fds[i] = p->wminfo->fds[i-1];
		}
		p->wminfo->wmapinfo->addr[found] = found == 0 ? 0 : p->wminfo->wmapinfo->addr[found-1] + p->wminfo->wmapinfo->length[found-1];
		p->wminfo->wmapinfo->length[found] = length;
	}
	if(found == -1) return FAILED;
	  p->wminfo->wmapinfo->total_mmaps++;
  	p->wminfo->wmapinfo->n_loaded_pages[found] = 0; 
  	p->wminfo->flags[found] = flags;
  	p->wminfo->fds[found] = !(flags & MAP_ANONYMOUS) ? f : 0;
  	p->wminfo->wmapinfo->total_mmaps++;
	
    return addr;
}

int 
sys_wunmap(void){
	uint addr;
	pte_t *pte;

	if(argint(0, (int*) &addr) + argint(1, (int*) &pte)){
		return FAILED;
	}
	while(*(pte = walkpgdir(myproc()->pgdir, (void *) addr, 0)) & PTE_P){
		//kfree(P2V(PTE_ADDR(*pte)));
		*pte = 0;
	}
	return addr;
}

uint 
sys_wremap(void){
	uint oldaddr;
  int oldsize;
	int newsize;
	int flags;
	if(argint(0, (int*) &oldaddr) + argint(1, &oldsize) + argint(2, &newsize) + argint(3, &flags) < 0){
		return FAILED;
	}
	return SUCCESS;
}

int 
sys_getpgdirinfo(void){
	struct pgdirinfo* pdinfo;
  	if(argptr(0, (void*)&pdinfo, sizeof(pdinfo)) < 0){
    	return FAILED;
 	}
	struct proc* p = myproc();
	pdinfo->n_upages = p->pginfo->n_upages;
  for(int i = 0; i < pdinfo->n_upages; i++){
    pdinfo->pa[i] = p->pginfo->pa[i];
    pdinfo->va[i] = p->pginfo->va[i];
  }
	return SUCCESS;
}

int 
sys_getwmapinfo(void){
	struct wmapinfo* wminfo;
	if(argptr(0, (char **)&wminfo, sizeof(wminfo)) < 0){
		return FAILED;
	}
	struct proc* p = myproc();
	wminfo->total_mmaps = p->wminfo->wmapinfo->total_mmaps;
  for(int i = 0; i < wminfo->total_mmaps; i++){
    wminfo->addr[i] = p->wminfo->wmapinfo->addr[i];
    wminfo->length[i] = p->wminfo->wmapinfo->length[i];
    wminfo->n_loaded_pages[i] = p->wminfo->wmapinfo->n_loaded_pages[i];
  }
	return SUCCESS;
}