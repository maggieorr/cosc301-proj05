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

typedef struct Node{
  int refs;
  struct Node *next;
} Node;



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
	    return;
	}

	if (dirent->deName[0] == SLOT_DELETED) 
	{
	    /* we found a deleted entry - we can just overwrite it */
	    write_dirent(dirent, filename, start_cluster, size);
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
               
               
        int fat_chain = 0;
        uint16_t next_cluster = getushort(dirent->deStartCluster);
        //set original cluster
        uint16_t orig_cluster = next_cluster;
        uint16_t previous;
        while(is_valid_cluster(next_cluster,bpb)){
            refs[next_cluster]++;
            //check for when refs[next_cluster] >1
            //what would you fix in this case?
            if (refs[next_cluster] > 1){
            		//do something
            }
            uint16_t previous = next_cluster;
            next_cluster = get_fat_entry(next_cluster,image_buf, bpb);
            //printf("clusta clusta: %d\n", next_cluster);
        		if (previous==next_cluster){
        			printf("pointing to itself\n");
        			//mark previous as EOF and leave 
        			set_fat_entry(next_cluster, FAT12_MASK & CLUST_EOFS,image_buf, bpb);
        			fat_chain++;
        			break;
        		}
        		if (next_cluster == (FAT12_MASK & CLUST_BAD)){
        			printf("BAD CLUSTER\n");
        			//next_cluster= get_fat_entry(next_cluster,image_buf, bpb); 
        			set_fat_entry(previous,FAT12_MASK & CLUST_EOFS,image_buf, bpb);
        			//do we have to free the bad cluster???????
        			printf("%d\n", previous);
        			printf("%d\n", next_cluster);
        			printf("%d\n", get_fat_entry(next_cluster,image_buf, bpb));
        			break;
        		}
       
        		
						fat_chain ++;
        }
        int getsize;
        if (size%512 == 0)
        	getsize = size/512;
        else
        	getsize= size/512 + 1;
        	
        if (getsize< fat_chain){
        			printf("CONSISTENCY PROBLEM!! file size is less than the cluster chain length\n");
        			//need to free clusters
        			//orig_cluster + getsize
        			next_cluster = get_fat_entry(orig_cluster+getsize-1, image_buf, bpb);
        			while(is_valid_cluster(next_cluster,bpb)){
        				previous = next_cluster;
        				set_fat_entry(previous, FAT12_MASK & CLUST_FREE, image_buf, bpb);
        				next_cluster = get_fat_entry(next_cluster, image_buf, bpb);
        			}
        			set_fat_entry(orig_cluster+getsize-1, FAT12_MASK & CLUST_EOFS, image_buf, bpb);
        }
        			
        if (getsize > fat_chain){
        			printf("CONSISTENCY PROBLEM!! file size is greater than the cluster chain length\n");
        			int new_size= fat_chain*512;
        			putulong(dirent->deFileSize,new_size);
        }
               
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

void findorphans(int *refs, int numsec, uint8_t *image_buf, struct bpb33* bpb){
		int orphans=0;
		for(int i=2;i<numsec;i++){
			uint16_t cluster = get_fat_entry(i,image_buf, bpb);
			if (refs[i]==0 && cluster != (FAT12_MASK & CLUST_FREE) && cluster != (FAT12_MASK & CLUST_BAD)){ 
				orphans++;
				//create_dirent

			}
		}
		printf("orphan bebes: %d\n", orphans);
}
//make new entry in sector for the orphan...
//dos copy
//or bad

//maximum size = 3*512 min=512*2 -----1024 up to 1536
//check if size is consistent with number of references found
//walk through disk image and fix anomalies
//two file entries with same starting cluster
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
    
    //int clusters = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
    int numsec= bpb->bpbSectors;
    int *refs = malloc(sizeof(int)*bpb->bpbSectors);
    for(int i = 0; i<bpb->bpbSectors; i++){
      refs[i]=0;
    }
    
    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);
    traverse_root(image_buf, bpb, refs);
    findorphans(refs, numsec, image_buf, bpb);



    unmmap_file(image_buf, &fd);
    return 0;
}
