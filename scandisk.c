#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

void write_dirent(struct direntry *dirent, char *filename, 
		  uint16_t start_cluster, uint32_t size)
{
    char *p, *p2;
    char *uppername;
    int len, i;

    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));

    /* extract just the filename part */
    uppername = strdup(filename);
    p2 = uppername;
    for (i = 0; i < strlen(filename); i++) 
    {
	if (p2[i] == '/' || p2[i] == '\\') 
	{
	    uppername = p2+i+1;
	}
    }

    /* convert filename to upper case */
    for (i = 0; i < strlen(uppername); i++) 
    {
	uppername[i] = toupper(uppername[i]);
    }

    /* set the file name and extension */
    memset(dirent->deName, ' ', 8);
    p = strchr(uppername, '.');
    memcpy(dirent->deExtension, "___", 3);
    if (p == NULL) 
    {
	fprintf(stderr, "No filename extension given - defaulting to .___\n");
    }
    else 
    {
	*p = '\0';
	p++;
	len = strlen(p);
	if (len > 3) len = 3;
	memcpy(dirent->deExtension, p, len);
    }

    if (strlen(uppername)>8) 
    {
	uppername[8]='\0';
    }
    memcpy(dirent->deName, uppername, strlen(uppername));
    free(p2);

    /* set the attributes and file size */
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, start_cluster);
    putulong(dirent->deFileSize, size);

}



void create_dirent(struct direntry *dirent, char *filename, 
		   uint16_t start_cluster, uint32_t size,
		   uint8_t *image_buf, struct bpb33* bpb)
{
    while (1) 
    {
	if (dirent->deName[0] == SLOT_EMPTY) 
	{
	    /* we found an empty slot at the end of the directory */
	    write_dirent(dirent, filename, start_cluster, size);
	    dirent++;

	    /* make sure the next dirent is set to be empty, just in
	       case it wasn't before */
	    memset((uint8_t*)dirent, 0, sizeof(struct direntry));
	    dirent->deName[0] = SLOT_EMPTY;
	    printf("here1\n");
	    return;
	}

	if (dirent->deName[0] == SLOT_DELETED) 
	{
	    /* we found a deleted entry - we can just overwrite it */
	    write_dirent(dirent, filename, start_cluster, size);
	    printf("here2\n");
	    return;
	}
	dirent++;
    }
}

void print_indent(int indent)
{
    int i;
    for (i = 0; i < indent*4; i++)
	printf(" ");
}

//A function to find a few errors that might be wrong with the files and their clusters
//errors include: inconsistency problems, bad or free clusters being pointed to, and 
//a cluster referencing itself (therefore creating an infinite chain).
//The function will also fix all of these problems.
void check_errors(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb, int *refs, int size){
				//keep track of the size of the FAT entry chain
				int fat_chain = 0;
        uint16_t next_cluster = getushort(dirent->deStartCluster);
        //set original cluster
        uint16_t orig_cluster = next_cluster;
        uint16_t previous;
        //go through chain, update the reference array, and find & fix errors
        while(is_valid_cluster(next_cluster,bpb)){
            refs[next_cluster]++;
            uint16_t previous = next_cluster;
            next_cluster = get_fat_entry(next_cluster,image_buf, bpb);
        		if (previous==next_cluster){
        			printf("Pointing to itself - Setting FAT entry to EOF\n");
        			//mark as EOF and leave 
        			set_fat_entry(next_cluster, FAT12_MASK & CLUST_EOFS,image_buf, bpb);
        			fat_chain++;
        			break;
        		}
        		if (next_cluster == (FAT12_MASK & CLUST_BAD)){
        			printf("BAD CLUSTER!! Set previous cluster to EOF\n");
        			//mark as end of file
        			set_fat_entry(previous,FAT12_MASK & CLUST_EOFS,image_buf, bpb);
        			break;
        		}
        		if (next_cluster == (FAT12_MASK & CLUST_FREE)){
        			set_fat_entry(previous,FAT12_MASK & CLUST_EOFS,image_buf, bpb);
        			break;
        		}        		
						fat_chain ++;
        }
        
        int getsize;
        if (size%512 == 0)
        	getsize = size/512;
        else
        	getsize= size/512 + 1;
        	
        //check for size inconsistencies between the size in the directory and the chain of FAT entries 
        //and fix them when necessary
        
        if (getsize< fat_chain){
        			printf("CONSISTENCY PROBLEM!! file size is less than the cluster chain length\n");
        			//need to free cluster and the chain of clusters after it
        			next_cluster = get_fat_entry(orig_cluster+getsize-1, image_buf, bpb);
        			while(is_valid_cluster(next_cluster,bpb)){
        				previous = next_cluster;
        				set_fat_entry(previous, FAT12_MASK & CLUST_FREE, image_buf, bpb);
        				next_cluster = get_fat_entry(next_cluster, image_buf, bpb);
        			}
        			//set original cluster to end of file
        			set_fat_entry(orig_cluster+getsize-1, FAT12_MASK & CLUST_EOFS, image_buf, bpb);
        }
        			
        if (getsize > fat_chain){
        			printf("CONSISTENCY PROBLEM!! file size is greater than the cluster chain length\n");
        			int new_size= fat_chain*512;
        			putulong(dirent->deFileSize,new_size);
        }
}

//modify print_dirent 
//only goes through directories, want it to print out for files
uint16_t print_dirent(struct direntry *dirent, int indent, uint8_t *image_buf, struct bpb33* bpb, int *refs)
{
    uint16_t followclust = 0;
    int i;
    char name[9];
    char extension[4];
    uint32_t size;
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    if (name[0] == SLOT_EMPTY)
    {
	    return followclust;
    }

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED)
    {
	    return followclust;
    }

    if (((uint8_t)name[0]) == 0x2E)
    {
	// dot entry ("." or "..")
	// skip it
        return followclust;
    }

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--) 
    {
	      if (name[i] == ' ') 
	          name[i] = '\0';
	      else 
	          break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--) 
    {
	      if (extension[i] == ' ') 
	          extension[i] = '\0';
	      else 
	          break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN)
    {
	      // ignore any long file name extension entries
	      //
	      // printf("Win95 long-filename entry seq 0x%0x\n", dirent->deName[0]);
    }
    else if ((dirent->deAttributes & ATTR_VOLUME) != 0) 
    {
	      printf("Volume: %s\n", name);
    } 
    else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0) 
    {
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
	     if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN)
       {
	        print_indent(indent);
        	    printf("%s/ (directory)\n", name);
                file_cluster = getushort(dirent->deStartCluster);
                followclust = file_cluster;
       }
    }
    else 
    {
        /*
         * a "regular" file entry
         * print attributes, size, starting cluster, etc.
         */
	      int ro = (dirent->deAttributes & ATTR_READONLY) == ATTR_READONLY;
	      int hidden = (dirent->deAttributes & ATTR_HIDDEN) == ATTR_HIDDEN;
	      int sys = (dirent->deAttributes & ATTR_SYSTEM) == ATTR_SYSTEM;
	      int arch = (dirent->deAttributes & ATTR_ARCHIVE) == ATTR_ARCHIVE;

	      size = getulong(dirent->deFileSize);
	      print_indent(indent);
	      printf("%s.%s (%u bytes) (starting cluster %d) %c%c%c%c\n", 
	             name, extension, size, getushort(dirent->deStartCluster),
	             ro?'r':' ', 
               hidden?'h':' ', 
               sys?'s':' ', 
               arch?'a':' ');
        //added new function to check for any errors that could be fixed within the
        //cluster chain of FAT entries       
        check_errors(dirent, image_buf, bpb, refs, size);              
    }

    return followclust;
}


void follow_dir(uint16_t cluster, int indent, uint8_t *image_buf, struct bpb33* bpb, int *refs)
{
    while (is_valid_cluster(cluster, bpb))
    {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
	for ( ; i < numDirEntries; i++)
	{
            
            uint16_t followclust = print_dirent(dirent, indent,image_buf,bpb,refs);
            if (followclust){
                refs[followclust]++;
                follow_dir(followclust, indent+1, image_buf, bpb, refs);
            }
            dirent++;
	}

	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}


void traverse_root(uint8_t *image_buf, struct bpb33* bpb, int *refs)
{
    uint16_t cluster = 0;

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        uint16_t followclust = print_dirent(dirent, 0, image_buf, bpb, refs);
        if (is_valid_cluster(followclust, bpb)){
            refs[followclust]++;
            follow_dir(followclust, 1, image_buf, bpb, refs);
        }
        dirent++;
    }
}



void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}

//a function to create a new file in the directory for all of the orphans
//creates the string for the filename and then puts it into the root directory

void create_file(int orphans, int size, int i, uint8_t *image_buf, struct bpb33* bpb){
				char string[5];
				sprintf(string, "%d", orphans);
				char filename[1024]="";
				strcat(filename, "found");
				strcat(filename, string);
				strcat(filename, ".dat\0");
				char *file = filename;
				printf("New file to to the driectory add is: %s\n", filename);
				printf("Orphan has a chain of %d clusters\n", size);
				struct direntry *dirent = (struct direntry*)root_dir_addr(image_buf, bpb);
				create_dirent(dirent, file, i, size*512, image_buf, bpb);
}


//a function to search for orphans (clusters that have no reference but are marked as bad or free)
//and save them (aka add a new file to the directory and include its chain of FAT entries)

void findorphans(int *refs, int numsec, uint8_t *image_buf, struct bpb33* bpb){
		int orphans=0;
		//go through the ref array and find any orphans
		for(int i=2;i<numsec;i++){
			uint16_t cluster = get_fat_entry(i,image_buf, bpb);
			if (refs[i]==0 && cluster != (FAT12_MASK & CLUST_FREE) && cluster != (FAT12_MASK & CLUST_BAD)){ 
				printf("Found orphan at: %d\n",i);
				orphans++;
				int size=1;
				refs[i]=1;
				uint16_t copy = cluster;
				
				while(is_valid_cluster(copy, bpb)){
					copy= get_fat_entry(copy,image_buf, bpb);
					refs[copy]++;
          
          if (refs[copy] > 1){
          	  struct direntry *dirent = (struct direntry*)cluster_to_addr(copy,image_buf, bpb);
		          //delete second entry that comes along if count will be greater than 1
		          dirent->deName[0] = SLOT_DELETED;
		          refs[copy] --;
		          printf("\nLotso refs - deleting the extra ones\n");
           }
					size++;
				}
				create_file(orphans, size, i, image_buf, bpb);
			}
		}
		
		printf("total orphan bebes: %d\n", orphans);
}

int main(int argc, char** argv) {
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc < 2) {
	usage(argv[0]);
    }

    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);

    // your code should start here...
    
    int numsec= bpb->bpbSectors;
    int *refs = malloc(sizeof(int)*bpb->bpbSectors);
    //initialize all reference counts to 0
    for(int i = 0; i<bpb->bpbSectors; i++){
      refs[i]=0;
    }
    
    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);
    //go through each cluster in the directory and their chains and then find possible size errors 
    traverse_root(image_buf, bpb, refs);
    //find and fix all orphans
    findorphans(refs, numsec, image_buf, bpb);

    unmmap_file(image_buf, &fd);
    return 0;
}
