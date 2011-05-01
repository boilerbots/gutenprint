/******************************************************************************
 * pixma_parse.c parser for Canon BJL printjobs 
 * Copyright (c) 2005 - 2007 Sascha Sommer <saschasommer@freenet.de>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 *****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>


#define MAX_COLORS 36  /* was: 8 maximum number of Colors: CMYKcmyk */

#include "pixma_parse.h"

/*TODO: 
  1. change color loops to search for each named color rather than using a predefined order.
  2. keep iP6700 workaround, but check what happens with the real printer.
*/



/* nextcmd(): find a command in a printjob
 * commands in the printjob start with either ESC[ or ESC(
 * ESC@ (go to neutral mode) and 0xc (form feed) are handled directly 
 * params:
 *  infile: handle to the printjob
 *  cmd: the command type
 *  buf: the buffer to hold the arguments, has to be at least 0xFFFF bytes
 *  cnt: len of the arguments
 *
 * return values:
 *   0 when a command has been successfully read
 *   1 when EOF has been reached
 *  -1 when an error occurred
 */
static int nextcmd( FILE *infile,unsigned char* cmd,unsigned char *buf, unsigned int *cnt, unsigned int *xml_read)
{
	unsigned char c1,c2;
	unsigned int startxml, endxml;
	unsigned int xmldata;
	if (feof(infile))
		return -1;
	while (!feof(infile)){
		c1 = fgetc(infile);
		if(feof(infile)) /* NORMAL EOF */
			return 1;
		/* add skip for XML header and footer */
		if (c1 == 60 ){  /* "<" for XML start */
		  if (*xml_read==0){
		    /* start: */
		    startxml=680;
		    xmldata=fread(buf,1,679,infile); /* 1 less than 680 */
		    printf("nextcmd: read starting XML %d %d\n", *xml_read, startxml);
		    *xml_read=1;
		  }else if (*xml_read==1) {
		    /* end */
		    endxml=263;
		    xmldata=fread(buf,1,262,infile); /* 1 less than 263*/
		    printf("nextcmd: read ending XML %d %d\n", *xml_read, endxml);
		    *xml_read=2;
		  }
		  /* no alternatives yet */
		}else if (c1 == 27 ){  /* A new ESC command */
			c2 = fgetc(infile);
			if(feof(infile))
				return 1;
			if (c2=='[' || c2=='(' ){   /* ESC[ or ESC( command */
				*cmd = fgetc(infile);    /* read command type  (1 byte) */
				if(feof(infile))
					return 1;
				c1 = fgetc(infile); /* read size 16 bit little endian */
				c2 = fgetc(infile);
				*cnt = c1 + (c2<<8);
				if (*cnt){  /* read arguments */
					unsigned int read;
					if((read=fread(buf,1,*cnt,infile)) != *cnt){
						printf("nextcmd: read error - not enough data %d %d\n", read, *cnt);
						return -1;
					}
				}
				return 0;
			}else if(c2 == '@'){ /* ESC@ */
				printf("ESC @ Return to neutral mode\n");
			} else {
				printf("unknown byte following ESC %x \n",c2);
			}
		}else if(c1==0x0c){ /* Form Feed */
			printf("-->Form Feed\n");
		}else{
			printf("UNKNOWN BYTE 0x%x @ %lu\n",c1,ftell(infile));
		}
	}
	return -1;
}


/* return pointer to color info structure matching name */
static color_t* get_color(image_t* img,char name){
	int i;
	for(i=0;i<MAX_COLORS;i++)
		if(img->color[i].name==name)
			return &(img->color[i]);
	return NULL;
}

static int valid_color(unsigned char color){
	int i;
	for(i=0;i<sizeof(valid_colors) / sizeof(valid_colors[0]);i++)
		if(valid_colors[i] == color)
			return 1;
	printf("unknown color %c 0x%x\n",color,color);
	return 0;
}


/* eight2ten() 
 * decompression routine for canons 10to8 compression that stores 5 3-valued pixels in 8-bit
 */
static int eight2ten(unsigned char* inbuffer,unsigned char* outbuffer,int num_bytes,int outbuffer_size){
	PutBitContext s;
	int read_pos=0;
	init_put_bits(&s, outbuffer,outbuffer_size);
	while(read_pos < num_bytes){
		unsigned short value=Table8[inbuffer[read_pos]];
		++read_pos;
		put_bits(&s,10,value);
	}
	return s.buf_ptr-s.buf;
}


/* reads a run length encoded block of raster data, decodes and uncompresses it */
static int Raster(image_t* img,unsigned char* buffer,unsigned int len,unsigned char color_name,unsigned int maxw){
	color_t* color=get_color(img,color_name);
        char* buf = (char*)buffer;
	int size=0; /* size of unpacked buffer */
	int cur_line=0; /* line relative to block begin */

	unsigned char* dst=malloc(len*256); /* the destination buffer */
	unsigned char* dstr=dst;
	if(!color){
          printf("no matching color for %c (0x%x, %i) in the database => ignoring %i bytes\n",color_name,color_name,color_name, len);
	}

	/* decode pack bits */
	while( len > 0){
		int c = *buf;
		++buf;
		--len;
		if(c >= 128)
			c -=256;
		if(c== -128){ /* end of line => decode and copy things here */
			/* create new list entry */
			if(color && size){
				if(!color->tail)
					color->head = color->tail = color->pos = calloc(1,sizeof(rasterline_t));
				else {
					color->head->next = calloc(1,sizeof(rasterline_t));
					color->head=color->head->next;
				}
				color->head->line = img->height + cur_line;
				if(!color->compression){
					color->head->buf=calloc(1,size+8); /* allocate slightly bigger buffer for get_bits */
					memcpy(color->head->buf,dstr,size);
					color->head->len=size;
				}else{ /* handle 5pixel in 8 bits compression */
					color->head->buf=calloc(1,size*2+8);
					size=color->head->len=eight2ten(dstr,color->head->buf,size,size*2);
				}
			}
			/* adjust the maximum image width */
			if(color && color->bpp && img->width < size*8/color->bpp){
				unsigned int newwidth = size * 8 / color->bpp;
				if(maxw && newwidth > maxw)
					newwidth = maxw;
				img->width = newwidth;
			}
			/* reset output buffer */
			size=0;
			dst=dstr;
			++cur_line;
		}else {
			int i;
			if(c < 0){ /* repeat char */
				i= - c + 1;
				c=*buf;
				++buf;
				--len;
				memset(dst,c,i);
				dst +=i;
				size+=i;
			}else{ /* normal code */
				i=c+1;
				len-=i;
				memcpy( dst, buf, i);
				buf +=i;
				dst +=i;
				size+=i;
			}
		}
	}
	free(dstr);
	return 0;
}


/* checks if the buffer contains a pixel definition at the given x and y position */
static inline int inside_range(color_t* c,int x,int y){
	if(c->bpp && c->pos &&  c->pos->line==y && c->pos->len && (c->pos->len *8 >= c->bpp*x))
		return 1;
	return 0;
}


/* moves all iterators in the rasterline list */
static void advance(image_t* img,unsigned int to){
	int i;
	for(i=0;i<MAX_COLORS;i++){
		while(img->color[i].pos && img->color[i].pos->line < to && img->color[i].pos->next && img->color[i].pos->next->line <= to){
			img->color[i].pos = img->color[i].pos->next;
			if(!img->color[i].pos->next)
				break;
		}
	}
}

/* write 1 line of decoded raster data */
static void write_line(image_t*img,FILE* fp,int pos_y){
	int i;
	unsigned int x;
	unsigned int written;
	unsigned char* line=malloc(img->width*3);
	color_t* C=get_color(img,'C');
	color_t* M=get_color(img,'M');
	color_t* Y=get_color(img,'Y');
	color_t* K=get_color(img,'K');
	color_t* c=get_color(img,'c');
	color_t* m=get_color(img,'m');
	color_t* y=get_color(img,'y');
	color_t* k=get_color(img,'k');
	color_t* H=get_color(img,'H');
	color_t* R=get_color(img,'R');
	color_t* G=get_color(img,'G');
	/* color_t* A=get_color(img,'A'); */
	/* color_t* B=get_color(img,'B'); */
	/* color_t* D=get_color(img,'D'); */
	/* color_t* E=get_color(img,'E'); */
	/* color_t* F=get_color(img,'F'); */
	/* color_t* I=get_color(img,'I'); */
	/* color_t* J=get_color(img,'J'); */
	/* color_t* L=get_color(img,'L'); */
	/* color_t* N=get_color(img,'N'); */
	/* color_t* O=get_color(img,'O'); */
	/* color_t* P=get_color(img,'P'); */
	/* color_t* Q=get_color(img,'Q'); */
	/* color_t* S=get_color(img,'S'); */
	/* color_t* T=get_color(img,'T'); */
	/* color_t* U=get_color(img,'U'); */
	/* color_t* V=get_color(img,'V'); */
	/* color_t* W=get_color(img,'W'); */
	/* color_t* X=get_color(img,'X'); */
	/* color_t* Z=get_color(img,'Z'); */
	/* color_t* a=get_color(img,'a'); */
	/* color_t* b=get_color(img,'b'); */
	/* color_t* d=get_color(img,'d'); */
	/* color_t* e=get_color(img,'e'); */
	/* color_t* f=get_color(img,'f'); */
	GetBitContext gb[MAX_COLORS];
	/* move iterator */
	advance(img,pos_y);
	/* init get bits */
	for(i=0;i<MAX_COLORS;i++){
		if(inside_range(&(img->color[i]),0,pos_y)){
			init_get_bits(&(gb[i]),img->color[i].pos->buf,img->color[i].pos->len);
		}
	}
	for(x=0;x<img->width;x++){
		int lK=0,lM=0,lY=0,lC=0;
		/* get pixel values */
		for(i=0;i<MAX_COLORS;i++){
		  if(inside_range(&img->color[i],x,pos_y))
		    img->color[i].value = get_bits(&gb[i],img->color[i].bpp);
		  else
		    img->color[i].value = 0;
		  /* update statistics */
		  (img->color[i].dots)[img->color[i].value] += 1;
		  /* set to 1 if the level is used */	
		  (img->color[i].usedlevels)[img->color[i].value]=1;
		}
		/* calculate CMYK values */
		lK=K->density * K->value/(K->level-1) + k->density * k->value/(k->level-1);
		lM=M->density * M->value/(M->level-1) + m->density * m->value/(m->level-1);
		lY=Y->density * Y->value/(Y->level-1) + y->density * y->value/(y->level-1);
		lC=C->density * C->value/(C->level-1) + c->density * c->value/(c->level-1);

		/* detect image edges */
		if(lK || lM || lY || lC){
			if(!img->image_top)
				img->image_top = pos_y;
			img->image_bottom = pos_y;
			if(x < img->image_left)
				img->image_left = x;
			if(x > img->image_right)
				img->image_right = x;
		}
			
                /* clip values */
                if(lK > 255)
                   lK = 255;
                if(lM > 255)
                   lM = 255;
                if(lC > 255)
                   lC = 255;
                if(lY > 255)
                   lY = 255;
		/* convert to RGB */
		/* 0 == black, 255 == white */
		line[x*3]=255 - lC - lK;        
		line[x*3+1]=255 - lM -lK;      
		line[x*3+2]=255 - lY -lK;     
		++img->dots;
	}

	/* output line */
	if((written = fwrite(line,img->width,3,fp)) != 3)
		printf("fwrite failed %u vs %u\n",written,img->width*3);
	free(line);
}


/* create a ppm image from the decoded raster data */
static void write_ppm(image_t* img,FILE* fp){
	int i;
	/* allocate buffers for dot statistics */
        for(i=0;i<MAX_COLORS;i++){
	  /*img->color[i].dots=calloc(1,sizeof(int)*(img->color[i].level+1));*/
	  img->color[i].dots=calloc(1,sizeof(int)*(1<<(img->color[i].bpp)+1));
	}	
	/* allocate buffers for levels used*/
        for(i=0;i<MAX_COLORS;i++){
	  img->color[i].usedlevels=calloc(1,sizeof(int)*(1<<(img->color[i].bpp)+1));
	}	

	/* write header */
	fputs("P6\n", fp);
	fprintf(fp, "%d\n%d\n255\n", img->width, img->height);

	/* set top most left value */
	img->image_left = img->width;

	/* write data line by line */
	for(i=0;i<img->height;i++){
		write_line(img,fp,i);
	}
	
	/* output some statistics */
	printf("statistics:\n");
	for(i=0;i<MAX_COLORS;i++){
	  int level;
	  if (img->color[i].bpp > 0) {
	    /*for(level=0;level < img->color[i].level;level++)*/
	    for(level=0;level < 1<<(img->color[i].bpp);level++)
	      printf("color %c level %i dots %i\n",img->color[i].name,level,img->color[i].dots[level]);
	  }
	}
	printf("Level values actually used:\n");
	for(i=0;i<MAX_COLORS;i++){
	  int level;
	  if (img->color[i].bpp > 0) {
	    printf("color %c bpp %i available levels %i declared levels %i --- actual level values used:\n",img->color[i].name,img->color[i].bpp,1<<(img->color[i].bpp),img->color[i].level);
	    for(level=0;level < 1<<(img->color[i].bpp);level++)
	      printf("%i",img->color[i].usedlevels[level]);
	    printf("\n");
	  }
	}
	/* translate area coordinates to 1/72 in (the gutenprint unit)*/
	img->image_top = img->image_top * 72.0 / img->yres ;
	img->image_bottom = img->image_bottom * 72.0 / img->yres ;
	img->image_left = img->image_left * 72.0 / img->xres ;
	img->image_right = img->image_right * 72.0 / img->xres ;
	printf("top %u bottom %u left %u right %u\n",img->image_top,img->image_bottom,img->image_left,img->image_right);
	printf("width %u height %u\n",img->image_right - img->image_left,img->image_bottom - img->image_top);	

	/* clean up */
        for(i=0;i<MAX_COLORS;i++){
		if(img->color[i].dots)
			free(img->color[i].dots);
	}	

}

static unsigned int read_uint32(unsigned char* a){
        unsigned int value = ( a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3];
        return value;
}


/* process a printjob command by command */
static int process(FILE* in, FILE* out,int verbose,unsigned int maxw,unsigned int maxh){
	image_t* img=calloc(1,sizeof(image_t));
	unsigned char* buf=malloc(0xFFFF);
	int returnv=0;
	int i;
	unsigned int xml_read;
	xml_read=0;

	printf("------- parsing the printjob -------\n");
	while(!returnv && !feof(in)){
		unsigned char cmd;
		unsigned int cnt = 0;
		if((returnv = nextcmd(in,&cmd,buf,&cnt,&xml_read)))
			break;
		switch(cmd){
			case 'c':
				printf("ESC (c set media (len=%i):\n",cnt);
				printf(" model id %x bw %x",buf[0]>> 4,buf[0]&0xf);
				printf(" media %x",buf[1]);
				printf(" direction %x quality %x\n",(buf[2]>>4)&3 ,buf[2]&0xf);
				break;
			case 'K':
				if(buf[1]==0x1f){
					printf("ESC [K go to command mode\n");
					do{
						fgets((char*)buf,0xFFFF,in);
						printf(" %s",buf);
					}while(strcmp((char*)buf,"BJLEND\n"));
				}else if(cnt == 2 && buf[1]==0x0f){
					printf("ESC [K reset printer\n");
					img->width=0;
					img->height=0;
				}else
					printf("ESC [K unsupported param (len=%i): %x\n",cnt,buf[1]);
				break;
			case 'b':
				printf("ESC (b set data compression (len=%i): %x\n",cnt,buf[0]);
				break;
			case 'I':
				printf("ESC (I select data transmission (len=%i): ",cnt);
				if(buf[0]==0)printf("default");
				else if(buf[0]==1)printf("multi raster");
				else printf("unknown 0x%x %i",buf[0],buf[0]);
				printf("\n");
				break;
			case 'l':
				printf("ESC (l select paper loading (len=%i):\n",cnt);
				printf(" model id 0x%x ",buf[0]>>4);
				printf(" source 0x%x",buf[0]&15);
				printf(" media: %x",buf[1]);
				printf(" paper gap: %x\n",buf[2]);
				break;
			case 'd':
				img->xres = (buf[0]<<8)|buf[1];
				img->yres = (buf[2]<<8)|buf[3];
				printf("ESC (d set raster resolution (len=%i): %i x %i\n",cnt,img->xres,img->yres);
				break;
			case 't':
				printf("ESC (t set image cnt %i\n",cnt);
				if(buf[0]>>7){
				        /* usual order */
				        char order[]="CMYKcmykHRGABDEFIJLMNOPQSTUVWXZabdef";
				        /* MP960 photo modes: k instead of K */
					/* char order[]="CMYkcmyKHRGABDEFIJLMNOPQSTUVWXZabdef";*/
					/* T-shirt transfer mode: y changed to k --- no y, no K */
					/*char order[]="CMYKcmkyHRGABDEFIJLMNOPQSTUVWXZabdef";*/
					/* MP990, MG6100, MG8100 plain modes */
				        /*char order[]="KCcMmYykRHGABDEFIJLMNOPQSTUVWXZabdef";*/
					/* MP990 etc. photo modes */
				        /* char order[]="KCcMmYykRHGABDEFIJLMNOPQSTUVWXZabdef"; */
					int black_found = 0;
					int num_colors = (cnt - 3)/3;
					printf(" bit_info: using detailed color settings for max %i colors\n",num_colors);
					if(buf[1]==0x80)
						printf(" format: BJ indexed color image format\n");
                                        else if(buf[1]==0x00)
						printf(" format: iP8500 flag set, BJ indexed color image format\n");
                                        else if(buf[1]==0x90)
						printf(" format: Pro9500 flag set, BJ indexed color image format\n");
					else{
						printf(" format: settings not supported 0x%x\n",buf[1]);
						/* returnv = -2; */
					}
					if(buf[2]==0x1)
					        printf(" ink: BJ indexed setting, also for iP8500 flag\n");
					else if(buf[2]==0x4)
					        printf(" ink: Pro series setting \n");
					else{
						printf(" ink: settings not supported 0x%x\n",buf[2]);
						/* returnv = -2; */
					}

					for(i=0;i<num_colors;i++){
					  if(i<MAX_COLORS){	
					    img->color[i].name=order[i];
					    img->color[i].compression=buf[3+i*3] >> 5;
					    img->color[i].bpp=buf[3+i*3] & 31;
					    img->color[i].level=(buf[3+i*3+1] << 8) + buf[3+i*3+2];/* check this carefully */
					    
					    /* work around for levels not matching (bpp gives more) */
					    /*if ((img->color[i].level == 3) && (img->color[i].bpp == 2)) {
					      printf("WARNING: color %c bpp %i declared levels %i, setting to 4 for testing \n",img->color[i].name,img->color[i].bpp,img->color[i].level);
					      img->color[i].level = 4;
					      } */
					    /*else if ((img->color[i].level == 4) && (img->color[i].bpp == 4)) {*/
					    /* levels is 16 but only each 2nd level is used */
					    /*  printf("WARNING: color %c bpp %i declared levels %i, setting to 16 for testing \n",img->color[i].name,img->color[i].bpp,img->color[i].level);
						img->color[i].level = 16;
						} */
					    
					    /* this is not supposed to give accurate images */
					    /* if(i<4) */ /* set to actual colors CMYK */
					    if((img->color[i].name =='K')||(img->color[i].name =='C')||(img->color[i].name =='M')||(img->color[i].name =='Y') ) 
					      img->color[i].density = 255;
					    else
					      img->color[i].density = 128; /*128+96;*/ /* try to add 0x80 to sub-channels for MP450 hi-quality mode */
					    if((order[i] == 'K' || order[i] == 'k') && img->color[i].bpp)
					      black_found = 1;
					    if(order[i] == 'y' && !black_found && img->color[i].level){
					      printf("iP6700 hack: treating color definition at the y position as k\n");
					      img->color[i].name = 'k';
					      order[i] = 'k';
					      order[i+1] = 'y';
					      black_found = 1;
					      img->color[i].density = 255;
					    } /* %c*/
					    printf(" Color %c Compression: %i bpp %i level %i\n",img->color[i].name,
						   img->color[i].compression,img->color[i].bpp,img->color[i].level);
					  }else{
					    printf(" Color %c Compression: %i bpp %i level %i\n",img->color[i].name,
						   img->color[i].compression,img->color[i].bpp,img->color[i].level);
					    /*printf(" Color ignoring setting %x %x %x\n",buf[3+i*3],buf[3+i*3+1],buf[3+i*3+2]);*/
					  }
					  
					}
					
					
				}else if(buf[0]==0x1 && buf[1]==0x0 && buf[2]==0x1){
					printf(" 1bit-per pixel\n");
					int num_colors = cnt*3; /*no idea yet! 3 for iP4000 */
					/*num_colors=9;*/
					/*for(i=0;i<MAX_COLORS;i++){*/
					for(i=0;i<num_colors;i++){
					  if(i<MAX_COLORS){	
					        /* usual */
					        const char order[]="CMYKcmykHRGABDEFIJLMNOPQSTUVWXZabdef";
						/* MP990, MG6100, MG8100 plain modes */
						/*const char order[]="KCcMmYykRHGABDEFIJLMNOPQSTUVWXZabdef";*/
						img->color[i].name=order[i];
						img->color[i].compression=0;
						img->color[i].bpp=1;
						img->color[i].level=2;
						img->color[i].density = 255;
						/*add color printout for this type also %c */
						printf(" Color %c Compression: %i bpp %i level %i\n",img->color[i].name,
						       img->color[i].compression,img->color[i].bpp,img->color[i].level);
					  }else{
					    printf(" Color %c Compression: %i bpp %i level %i\n",img->color[i].name,
						       img->color[i].compression,img->color[i].bpp,img->color[i].level);
						/*printf(" Color ignoring setting %x %x %x\n",buf[3+i*3],buf[3+i*3+1],buf[3+i*3+2]);*/
					  }
					}
				}else{
					printf(" bit_info: unknown settings 0x%x 0x%x 0x%x\n",buf[0],buf[1],buf[2]);
					/* returnv=-2; */
				}
				break;
			case 'L':
				printf("ESC (L set component order for F raster command (len=%i): ",cnt);
				img->color_order=calloc(1,cnt+1);
				/* check if the colors are sane => the iP4000 driver appends invalid bytes in the highest resolution mode */
				for(i=0;i<cnt;i++){
				  if (!valid_color(buf[i]))
				    if (!(valid_color(buf[i]-0x80))) {
				      printf("invalid color char [failed on initial]\n");
				      break;
				    }
				    else {
				      buf[i]=buf[i]-0x80;
				      printf("subtracting 0x80 from color char to give [corrected]: %c\n", buf[i]);
				    }
				  else
				    printf("found valid color char [fall-through]: %c\n",buf[i]);
				}
				cnt = i;
				memcpy(img->color_order,buf,cnt);
				printf("%s\n",img->color_order);
				img->num_colors = cnt;
				img->cur_color=0;
				break;
			case 'p':
				printf("ESC (p set extended margin (len=%i):\n",cnt);
                                printf(" printed length %i left %i\n",((buf[0]<<8 )+buf[1]) *6 / 5,(buf[2]<<8) + buf[3]);
                                printf(" printed width %i top %i\n",((buf[4]<<8 )+buf[5]) * 6 / 5,(buf[6]<<8) + buf[7]);

                                if(cnt > 8){
					int unit = (buf[12] << 8)| buf[13];
					int area_right = read_uint32(buf+14);
					int area_top = read_uint32(buf+18);
					unsigned int area_width = read_uint32(buf+22);
					unsigned int area_length = read_uint32(buf+26);
					int paper_right = read_uint32(buf+30);
					int paper_top = read_uint32(buf+34);
					unsigned int paper_width = read_uint32(buf+38);
					unsigned int paper_length = read_uint32(buf+42);
                                        printf(" unknown %i\n",read_uint32(buf+8));
                                        printf(" unit %i [1/in]\n",unit);
                                        printf(" area_right %i %.1f mm\n",area_right,area_right * 25.4 / unit);
                                        printf(" area_top %i %.1f mm\n",area_top,area_top * 25.4 / unit);
                                        printf(" area_width %u %.1f mm\n",area_width, area_width * 25.4 / unit);
                                        printf(" area_length %u %.1f mm\n",area_length,area_length * 25.4 / unit);
                                        printf(" paper_right %i %.1f mm\n",paper_right,paper_right * 25.4 / unit);
                                        printf(" paper_top %i %.1f mm\n",paper_top,paper_top * 25.4 / unit);
                                        printf(" paper_width %u %.1f mm\n",paper_width,paper_width * 25.4 / unit);
                                        printf(" paper_length %u %.1f mm\n",paper_length,paper_length * 25.4 / unit);
					img->top = (float)area_top / unit;
					img->left = (float)area_top / unit;
                                }
				break;
			case '$':
				printf("ESC ($ set duplex (len=%i)\n",cnt);
				break;
			case 'J':
				printf("ESC (J select number of raster lines per block (len=%i): %i\n",cnt,buf[0]);
				img->lines_per_block=buf[0];
				break;
			case 'F':
				if(verbose)
					printf("ESC (F raster block (len=%i):\n",cnt);
				if((returnv = Raster(img,buf,cnt,img->color_order[img->cur_color],maxw)))
					break;
				++img->cur_color;
				if(img->cur_color >= img->num_colors){
					img->cur_color=0;
					img->height+=img->lines_per_block;
				}
				break;
			case 'q':
				printf("ESC (q set page id (len=%i):%i\n",cnt,buf[0]);
				break;
			case 'r':
				printf("ESC (r printer specific command (len=%i): ",cnt);
				for(i=0;i<cnt;i++)
					printf("0x%x ",buf[i]);
				printf("\n");
				break;
			case 'A':
				if(verbose)
					printf("ESC (A raster line (len=%i): color %c\n",cnt,buf[0]);
				/* the single line rasters are not terminated by 0x80 => do it here
				 * instead of 0x80 every raster A command is followed by a 0x0d byte
				 * the selected color is stored in the first byte
				 */
				buf[cnt]=0x80;
				returnv = Raster(img,buf+1,cnt,buf[0],maxw);
				if (fgetc(in)!=0x0d){
					printf("Raster A not terminated by 0x0d\n");
					returnv=-4;
				}
				break;
			case 'e': /* Raster skip */
				if(verbose)
					printf("ESC (e advance (len=%i): %i\n",cnt,buf[0]*256+buf[1]);
				if(img->lines_per_block){
					img->height += (buf[0]*256+buf[1])*img->lines_per_block;
					img->cur_color=0;
				}else	
					img->height += (buf[0]*256+buf[1]);
				break;
			default: /* Last but not least completely unknown commands */
				printf("ESC (%c UNKNOWN (len=%i)\n",cmd,cnt);
				for(i=0;i<cnt;i++)
					printf(" 0x%x",buf[i]);
				printf("\n");

		}
	}

	printf("-------- finished parsing   --------\n");
	if(returnv < -2){ /* was < 0 :  work around to see what we get */
		printf("error: parsing the printjob failed error %i\n",returnv);
	} else {
	
		printf("created bit image with width %i height %i\n",img->width,img->height);
		if(maxh > 0){
			printf("limiting height to %u\n",maxh);
			img->height=maxh;
		}

		/* now that we have a complete nice raster image
	 	* lets build a bitmap from it
         	*/
		if(out)
			write_ppm(img,out);
		printf("dots: %u\n",img->dots);

	}
	/* deallocate resources */
	free(buf);
	for(i=0;i<MAX_COLORS;i++){
		rasterline_t** r=&img->color[i].tail;
		while(*r){
			rasterline_t* tmp=(*r)->next;
			free((*r)->buf);
			free(*r);
			*r=tmp;
		}
	}
	if(img->color_order)
		free(img->color_order);
	free(img);
	return returnv;
}



static void display_usage(void){
	printf("usage: pixma_parse [options] infile [outfile]\n");
	printf("infile: the printjob to parse\n");
	printf("outfile: if specified a ppm file will be generated from the raster data\n");
	printf("options:\n");
	printf(" -v: verbose print ESC e),F) and A) commands\n");
	printf(" -x width: cut the output ppm to the given width\n");
	printf(" -y height: cut the output ppm to the given height\n");
	printf(" -h: display this help\n");
}



int main(int argc,char* argv[]){
	int verbose = 0;
	unsigned int maxh=0;
	unsigned int maxw=0;
	char* filename_in=NULL,*filename_out=NULL;
	FILE *in,*out=NULL;
	int i;
	printf("pixma_parse - parser for Canon BJL printjobs (c) 2005-2007 Sascha Sommer <saschasommer@freenet.de>\n");

	/* parse args */
	for(i=1;i<argc;i++){
		if(strlen(argv[i]) >= 2 && argv[i][0] == '-'){
			if(argv[i][1] == 'v'){
				verbose = 1;
			}else if(argv[i][1] == 'h'){
				display_usage();
				return 0;	
			}else if(argv[i][1] == 'y'){
				if(argc > i+1){
					++i;
					maxh = atoi(argv[i]);
				}else{
					display_usage();
					return 1;
				}
			}else if(argv[i][1] == 'x'){
				if(argc > i+1){
					++i;
					maxw = atoi(argv[i]);
				}else{
					display_usage();
					return 1;
				}
			}else {
				printf("unknown parameter %s\n",argv[i]);
				return 1;
			}
		}else if(!filename_in){
			filename_in = argv[i];
		}else if(!filename_out){
			filename_out = argv[i];
		}else{
			display_usage();
			return 1;
		}
	}
	if(!filename_in){
		display_usage();
		return 1;
	}

	/* open input file */
	if(!(in=fopen(filename_in,"rb"))){
		printf("unable to open input file %s\n",filename_in);
		return 1;
	}

	/* open output file */
	if(filename_out && !(out=fopen(filename_out,"wb"))){
		printf("can't create the output file %s\n",filename_out);
		fclose(in);
		return 1;
	}
	
	/* process the printjob */
	process(in,out,verbose,maxw,maxh);

	/* cleanup */
	fclose(in);
	if(out)
		fclose(out);
	return 0;
} 
