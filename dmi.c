/* dmi.c  using the DMI from SMBIOS to read information about the hardware's
 * memory devices capabilities and where they are mapped into the address space
 *
 * Copyright (c) Joachim Deguara, AMD 2006
 *
 * Release under the GPL version 2
 * ----------------------------------------------------
 * Memtest86+ V1.70 - Added compliance with SMBIOS Spec V2.5
 *									- Support for FB-DIMM
 */

#include "test.h"
#include "stdint.h"


#define round_up(x,y) (((x) + (y) - 1) & ~((y)-1))
#define round_down(x,y) ((x) & ~((y)-1))


struct dmi_eps {
	uint8_t  anchor[4];
	int8_t   checksum;
	uint8_t  length;
	uint8_t  majorversion;
	uint8_t  minorversion;
	uint16_t maxstructsize;
	uint8_t  revision;
	uint8_t  pad[5];
	uint8_t  intanchor[5];
	int8_t   intchecksum;
	uint16_t tablelength;
	uint32_t tableaddress;
	uint16_t numstructs;
	uint8_t  SMBIOSrev;
} __attribute__((packed));

struct tstruct_header{
	uint8_t  type;
	uint8_t  length;
	uint16_t handle;
} __attribute__((packed));

struct mem_dev {
	struct tstruct_header header;
	uint16_t pma_handle;
	uint16_t err_handle;
	uint16_t tot_width;
	uint16_t dat_width;
	uint16_t size;
	uint8_t  form;
	uint8_t  set;
	uint8_t  dev_locator;
	uint8_t  bank_locator;
	uint8_t  type;
	uint16_t typedetail;
	uint16_t speed;
	uint8_t  manufacturer;
	uint8_t  serialnum;
	uint8_t  asset;
	uint8_t  partnum;
} __attribute__((packed));

struct md_map{
	struct tstruct_header header;
	uint32_t start;
	uint32_t end;
	uint16_t md_handle;
	uint16_t mama_handle;
	uint8_t  row_pos;
	uint8_t  interl_pos;
	uint8_t  interl_depth;
} __attribute__((packed));

struct pma{
	struct tstruct_header header;
	uint8_t  location;
	uint8_t  use;
	uint8_t  ecc;
	uint32_t capacity;
	uint16_t errhandle;
	uint16_t numdevs;
} __attribute__((packed));

static char *form_factors[] = {
	"?",
	"Other", "Unknown", "SIMM", "SIP", "Chip", "DIP", "ZIP",
	"Proprietary Card", "DIMM", "TSOP", "Row of chips", "RIMM",
	"SODIMM", "SRIMM", "FB-DIMM"
};

     // Slot 1 is "Other", according to SMBIOS 2.6.
     // BTW, SMBIOS 2.6 doesn't yet recognize DDR3 and I replaced Other to DDR3
     // Need fix as soon as SMBIOS will take DDR3 into consideration
static char *memory_types[] = {
	"?",
     	"DDR3", "Unknown", "DRAM", "EDRAM", "VRAM", "SRAM", "RAM",
	"ROM", "FLASH", "EEPROM", "FEPROM", "EPROM", "CDRAM", "3DRAM",
	"SDRAM", "SGRAM", "RDRAM", "DDR", "DDR2", "DDR2 FB"
};


struct mem_dev * mem_devs[MAX_DMI_MEMDEVS];
int mem_devs_count=0;
struct md_map * md_maps[MAX_DMI_MEMDEVS];
int md_maps_count=0;
int dmi_err_cnts[MAX_DMI_MEMDEVS];
short dmi_initialized=0;

int strlen(char * string){
	int i=0;
	while(*string++){i++;};
	return i;
}


char * get_tstruct_string(struct tstruct_header *header, int n){
	if(n<1)
		return 0;
	char * a = (char *)header + header->length;
	n--;
	do{
		if (!*a)
			n--;
		if (!n && *a)
			return a;
		a++;
	}while (!(*a==0 && *(a-1)==0));
	return 0;
}


int dmi_open(void){
	char *dmi, *dmi_search_start, *dmi_start;
	int i, found=0;
	struct dmi_eps *eps;
	char *table_start;
	int tstruct_count=0;

	for(i=0; i < MAX_DMI_MEMDEVS; i++)
		dmi_err_cnts[i]=-1;

	dmi_search_start = (char *)DMI_SEARCH_START;

	//find ancho
	for(dmi = dmi_search_start; dmi < dmi_search_start + 0xf0000; dmi +=16){
		if( *dmi == '_' &&
		    *(dmi+1) == 'S' &&
		    *(dmi+2) == 'M' &&
		    *(dmi+3) == '_'){
			found =1;
			break;
		}
	}
	if (!found) {
		return -1;
	}
	dmi_start=dmi;
	eps=(struct dmi_eps *)dmi;

	//check checksum
	int8_t checksum=0;
	for (; dmi < dmi_start + eps->length; dmi++)
		checksum += *dmi;
	if (checksum){
		return -1;
	}

	//we need at least revision 2.1 of SMBIOS
	if ( eps->majorversion < 2 &&
	     eps->minorversion < 1){
	    return -1;
	}


	table_start=(char *)eps->tableaddress;
	dmi=table_start;
//look at all structs
	while(dmi < table_start + eps->tablelength){
		struct tstruct_header *header = (struct tstruct_header *)dmi;
		if (header->type == 17) {
			mem_devs[mem_devs_count]=(struct mem_dev *)dmi;
			if (mem_devs[mem_devs_count]->size > 0){
				dmi_err_cnts[mem_devs_count] = 0;
			}
			mem_devs_count++;
		}
     		// Need fix (SMBIOS/DDR3)
     		if (header->type == 20 || header->type == 1)
			md_maps[md_maps_count++]=(struct md_map *)dmi;
		dmi+=header->length;
		while( ! (*dmi == 0  && *(dmi+1) == 0 ) )
			dmi++;
		dmi+=2;

		if (++tstruct_count > eps->numstructs)
			return -1;
	}
	dmi_initialized=1;
	return 0;
}

void print_dmi_info(void){
	int i,j,page;
	char * string=0;

	if(!dmi_initialized)
		dmi_open();

	if (mem_devs_count == 0){
		cprint(POP2_Y+1, POP2_X+2, "No valid DMI Memory Devices info found");
		while (get_key() == 0);
		return;
	}

	for(page=1; page <= 1 + (mem_devs_count-1)/8; page++){
		pop2clear();
		cprint(POP2_Y+1, POP2_X+2, "DMI Memory Device Info  (page ");
		itoa(string,page);
		cprint(POP2_Y+1, POP2_X+32, string);
		cprint(POP2_Y+1, POP2_X+33, "/");
		itoa(string,1 + (mem_devs_count-1)/8);
		cprint(POP2_Y+1, POP2_X+34, string);
		cprint(POP2_Y+1, POP2_X+35, ")");

		cprint(POP2_Y+3, POP2_X+4, "Location         Size(MB) Speed(MHz) Type    Form       Errors");
		cprint(POP2_Y+4, POP2_X+4, "--------------------------------------------------------------");

		for(i=8*(page-1); i<mem_devs_count && i<8*page; i++){
			int size_in_mb;
			int yof;

			yof=POP2_Y+5+2*(i-8*(page-1));
			cprint(yof, POP2_X+4, get_tstruct_string(&(mem_devs[i]->header), mem_devs[i]->dev_locator));

			if (mem_devs[i]->size == 0){
				cprint(yof, POP2_X+4+18, "Empty");
			}else if (mem_devs[i]->size == 0xFFFF){
				cprint(yof, POP2_X+4+18, "Unknown");
			}else{
				size_in_mb = 0xEFFF & mem_devs[i]->size;
				if (mem_devs[i]->size & 0x8000)
					size_in_mb = size_in_mb<<10;
				itoa(string, size_in_mb);
				cprint(yof, POP2_X+4+18, string);
			}
			
			//this is the only field that needs to be SMBIOS 2.3+ 
			if ( mem_devs[i]->speed && 
			     mem_devs[i]->header.length > 21){
				itoa(string, mem_devs[i]->speed);
				cprint(yof, POP2_X+4+27, string);
			}else{
				cprint(yof, POP2_X+4+27, "Unknown");
			}
			cprint(yof, POP2_X+4+37, memory_types[mem_devs[i]->type]);
			cprint(yof, POP2_X+4+45, form_factors[mem_devs[i]->form]);
			dprint(yof, POP2_X+4+55, dmi_err_cnts[i], 7, 0);

			//print mappings
			int mapped=0,of=0;
			cprint(yof+1, POP2_X+6,"mapped to: ");
			for(j=0; j<md_maps_count; j++){
				if (mem_devs[i]->header.handle != md_maps[j]->md_handle)
					continue;
				if (mapped++){
					cprint(yof+1, POP2_X+17+of, ",");
					of++;
				}
				hprint3(yof+1, POP2_X+17+of, md_maps[j]->start<<10, 12);
				of += 12;
				cprint(yof+1, POP2_X+17+of, "-");
				of++;
				hprint3(yof+1, POP2_X+17+of, md_maps[j]->end<<10, 12);
				of += 12;
			}
			if (!mapped)
				cprint(yof+1, POP2_X+17, "No mapping (Interleaved Device)");

		}

		wait_keyup();
		while (get_key() == 0);
	}
}
	
void add_dmi_err(ulong adr){
	int i,j;
	
	if(!dmi_initialized)
		dmi_open();
	
	for(i=0; i < md_maps_count; i++){
		if ( adr < (md_maps[i]->start<<10) ||
		     adr > (md_maps[i]->end<<10) )
			continue;

		//matching map found, now check find corresponding dev
		for(j=0; j < mem_devs_count; j++){
		    if (mem_devs[j]->header.handle == md_maps[i]->md_handle) {
			dmi_err_cnts[j]++;
			break;
		    }
		}
		break;
	}
}
