/*
    ZEsarUX  ZX Second-Emulator And Released for UniX
    Copyright (C) 2013 Cesar Hernandez Bano

    This file is part of ZEsarUX.

    ZEsarUX is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "cpu.h"
#include "esxdos_handler.h"
#include "operaciones.h"
#include "debug.h"
#include "menu.h"
#include "utils.h"
#include "diviface.h"

#if defined(__APPLE__)
	#include <sys/syslimits.h>
#endif


//Al leer directorio se usa esxdos_handler_root_dir y esxdos_handler_cwd
char esxdos_handler_root_dir[PATH_MAX]="";
char esxdos_handler_cwd[PATH_MAX]="";


//Usados al fopen de archivos y tambien al abrir directorios

struct s_esxdos_fopen {

	/* Para archivos */
	FILE *esxdos_last_open_file_handler_unix;
	//z80_byte temp_esxdos_last_open_file_handler;

	//Usado al hacer fstat
	struct stat last_file_buf_stat;


	/* Para directorios */
	//usados al leer directorio
	//z80_byte esxdos_handler_filinfo_fattrib;
	struct dirent *esxdos_handler_dp;
	DIR *esxdos_handler_dfd; //	=NULL;
	//ultimo directorio leido al listar archivos
	char esxdos_handler_last_dir_open[PATH_MAX];
	//Solo soporto abrir un directorio a la vez
	//z80_byte esxdos_handler_folder_handle;

	z80_byte buffer_plus3dos_header[8];
	z80_bit tiene_plus3dos_header;

	/* Comun */
	//Indica a 1 que el archivo/directorio esta abierto. A 0 si no
	z80_bit open_file;

	z80_bit is_a_directory;
};

struct s_esxdos_fopen esxdos_fopen_files[ESXDOS_MAX_OPEN_FILES];


z80_bit esxdos_handler_enabled={0};

//Retorna contador a array de estructura de archivo vacio. Retorna -1 si no hay
int esxdos_find_free_fopen(void)
{
	int i;

	for (i=0;i<ESXDOS_MAX_OPEN_FILES;i++) {
		if (esxdos_fopen_files[i].open_file.v==0) {
			printf ("Free handle: %d\n",i);
			return i;
		}
	}

	return -1;
}


void esxdos_handler_fill_size_struct(z80_int puntero,z80_long_int l)
{

	poke_byte_no_time(puntero++,l&0xFF);

	l=l>>8;
	poke_byte_no_time(puntero++,l&0xFF);

	l=l>>8;
	poke_byte_no_time(puntero++,l&0xFF);

	l=l>>8;
	poke_byte_no_time(puntero++,l&0xFF);
}


void esxdos_handler_fill_date_struct(z80_int puntero,z80_byte hora,z80_byte minuto,z80_byte doblesegundos,
			z80_byte dia,z80_byte mes,z80_byte anyo)
	{

//Fecha.
/*
22-23   Time (5/6/5 bits, for hour/minutes/doubleseconds)
24-25   Date (7/4/5 bits, for year-since-1980/month/day)
*/
z80_int campo_tiempo;

//       15 14 13 12 11 10 09 08 07 06 05 04 03 02 01 00
//       -- hora ------ --- minutos ----- -- doblesec --

campo_tiempo=(hora<<11)|(minuto<<5)|doblesegundos;


poke_byte_no_time(puntero++,campo_tiempo&0xFF);
poke_byte_no_time(puntero++,(campo_tiempo>>8)&0xff);

z80_int campo_fecha;

//       15 14 13 12 11 10 09 08 07 06 05 04 03 02 01 00
//       ----- anyo --------  --- mes --- ---- dia -----

campo_fecha=(anyo<<9)|(mes<<5)|dia;



poke_byte_no_time(puntero++,campo_fecha&0xFF);
poke_byte_no_time(puntero++,(campo_fecha>>8)&0xff);
}

void esxdos_handler_copy_hl_to_string(char *buffer_fichero)
{

	int i;

	for (i=0;peek_byte_no_time(reg_hl+i);i++) {
		buffer_fichero[i]=peek_byte_no_time(reg_hl+i);
	}

	buffer_fichero[i]=0;
}

void esxdos_handler_no_error_uncarry(void)
{
	Z80_FLAGS=(Z80_FLAGS & (255-FLAG_C));
}

void esxdos_handler_error_carry(z80_byte error)
{
	Z80_FLAGS |=FLAG_C;
	reg_a=error;
}

void esxdos_handler_return_call(void)
{
	reg_pc++;
}


//rellena fullpath con ruta completa
//funcion similar a zxpand_fileopen
void esxdos_handler_pre_fileopen(char *nombre_inicial,char *fullpath)
{


	//Si nombre archivo empieza por /, olvidar cwd
	if (nombre_inicial[0]=='/') sprintf (fullpath,"%s%s",esxdos_handler_root_dir,nombre_inicial);

	//TODO: habria que proteger que en el nombre indicado no se use ../.. para ir a ruta raiz inferior a esxdos_handler_root_dir
	else sprintf (fullpath,"%s/%s/%s",esxdos_handler_root_dir,esxdos_handler_cwd,nombre_inicial);


	int existe_archivo=si_existe_archivo(fullpath);



	//Si no existe buscar archivo sin comparar mayusculas/minusculas
	//en otras partes del codigo directamente se busca primero el archivo con util_busca_archivo_nocase,
	//aqui se hace al reves. Se busca normal, y si no se encuentra, se busca nocase


		if (!existe_archivo) {
			printf ("File %s not found. Searching without case sensitive\n",fullpath);
			char encontrado[PATH_MAX];
			char directorio[PATH_MAX];
			util_get_complete_path(esxdos_handler_root_dir,esxdos_handler_cwd,directorio);
			//sprintf (directorio,"%s/%s",esxdos_handler_root_dir,esxdos_handler_cwd);
			if (util_busca_archivo_nocase ((char *)nombre_inicial,directorio,encontrado) ) {
				printf ("Found with name %s\n",encontrado);
				existe_archivo=1;

				//cambiamos el nombre fullpath y el nombre_inicial por el encontrado
				sprintf ((char *)nombre_inicial,"%s",encontrado);

				sprintf (fullpath,"%s/%s/%s",esxdos_handler_root_dir,esxdos_handler_cwd,(char *)nombre_inicial);
				//sprintf (fullpath,"%s/%s/%s",esxdos_handler_root_dir,esxdos_handler_cwd,encontrado);
				printf ("Found file %s searching without case sensitive\n",fullpath);
			}
		}





}


void esxdos_handler_debug_file_flags(z80_byte b)
{
	if (b&ESXDOS_RST8_FA_READ) printf ("FA_READ|");
	if (b&ESXDOS_RST8_FA_WRITE) printf ("FA_WRITE|");
	if (b&ESXDOS_RST8_FA_OPEN_EX) printf ("FA_OPEN_EX|");
	if (b&ESXDOS_RST8_FA_OPEN_AL) printf ("FA_OPEN_AL|");
	if (b&ESXDOS_RST8_FA_CREATE_NEW) printf ("FA_CREATE_NEW|");
	if (b&ESXDOS_RST8_FA_USE_HEADER) printf ("FA_USE_HEADER|");

	printf ("\n");
}



void esxdos_handler_call_f_open(void)
{
	/*
	;                                                                       // Open file. A=drive. HL=Pointer to null-
;                                                                       // terminated string containg path and/or
;                                                                       // filename. B=file access mode. DE=Pointer
;                                                                       // to BASIC header data/buffer to be filled
;                                                                       // with 8 byte PLUS3DOS BASIC header. If you
;                                                                       // open a headerless file, the BASIC type is
;                                                                       // $ff. Only used when specified in B.
;                                                                       // On return without error, A=file handle.
*/

	char fopen_mode[10];

	z80_byte modo_abrir=reg_b;

	esxdos_handler_debug_file_flags(modo_abrir);

	//Si se debe escribir la cabecera plus3 . escribir en el sentido de:
	//Si se abre para lectura, se pasaran los 8 bytes del basic a destino DE
	//Si se abre para escritura, se leeran 8 bytes desde DE al archivo en cuanto se escriba la primera vez
	int debe_escribir_plus3_header=0;

	//Si se abre archivo para lectura
	int escritura=1;

	//Si debe escribir cabecera PLUS3DOS
	if ( (modo_abrir&ESXDOS_RST8_FA_USE_HEADER)==ESXDOS_RST8_FA_USE_HEADER) {
		//volcar contenido DE 8 bytes a buffer
		debe_escribir_plus3_header=1;
		modo_abrir &=(255-ESXDOS_RST8_FA_USE_HEADER);

	}

	//Modos soportados

	switch (modo_abrir) {
			case ESXDOS_RST8_FA_READ:
				strcpy(fopen_mode,"rb");
				escritura=0;
			break;

			case ESXDOS_RST8_FA_CREATE_NEW|ESXDOS_RST8_FA_WRITE:
				strcpy(fopen_mode,"w+b");
			break;

			//case FA_WRITE|FA_CREATE_NEW|FA_USE_HEADER

			default:

				printf ("Unsupported fopen mode: %02XH\n",reg_b);
				esxdos_handler_error_carry(ESXDOS_ERROR_EIO);
				esxdos_handler_return_call();
				return;
			break;
	}


	//Ver si no se han abierto el maximo de archivos y obtener handle libre
	int free_handle=esxdos_find_free_fopen();
	if (free_handle==-1) {
		esxdos_handler_error_carry(ESXDOS_ERROR_ENFILE);
		esxdos_handler_return_call();
		return;
	}

	char nombre_archivo[PATH_MAX];
	char fullpath[PATH_MAX];
	esxdos_handler_copy_hl_to_string(nombre_archivo);

	esxdos_fopen_files[free_handle].tiene_plus3dos_header.v=0;

	if (debe_escribir_plus3_header && escritura) {
		esxdos_fopen_files[free_handle].tiene_plus3dos_header.v=1;
		int i;
		for (i=0;i<8;i++) {
			z80_byte byte_leido=peek_byte_no_time(reg_de+i);
			esxdos_fopen_files[free_handle].buffer_plus3dos_header[i]=byte_leido;
			printf ("%02XH ",byte_leido);
		}

		printf ("\n");
	}


	esxdos_handler_pre_fileopen(nombre_archivo,fullpath);

	printf ("fullpath file: %s\n",fullpath);

	//printf ("file type: %d\n",get_file_type_from_name(fullpath));
	//sleep(5);

	//Si archivo es directorio, error
	if (get_file_type_from_name(fullpath)==2) {
		printf ("is a directory. can't fopen it\n");
		esxdos_handler_error_carry(ESXDOS_ERROR_EISDIR);
		esxdos_handler_return_call();
		return;
	}

	//Abrir el archivo.
	esxdos_fopen_files[free_handle].esxdos_last_open_file_handler_unix=fopen(fullpath,fopen_mode);


	if (esxdos_fopen_files[free_handle].esxdos_last_open_file_handler_unix==NULL) {
		esxdos_handler_error_carry(ESXDOS_ERROR_ENOENT);
		printf ("Error from esxdos_handler_call_f_open file: %s\n",fullpath);
		esxdos_handler_return_call();
		return;
	}
	else {
		//temp_esxdos_last_open_file_handler=1;
		if (debe_escribir_plus3_header && escritura==0) {
			printf ("Leyendo cabecera PLUS3DOS\n");
			char buffer_registros[1024];
			print_registers(buffer_registros);
			printf ("%s\n",buffer_registros);


			//Saltar los primeros 15
			char buffer_quince[15];
			fread(&buffer_quince,1,15,esxdos_fopen_files[free_handle].esxdos_last_open_file_handler_unix);

			//Y meter en DE los siguientes 8
			int i;
			z80_byte byte_leido;
			for (i=0;i<8;i++) {
				fread(&byte_leido,1,1,esxdos_fopen_files[free_handle].esxdos_last_open_file_handler_unix);
				poke_byte_no_time(reg_de+i,byte_leido);
				printf ("%02XH ",byte_leido);
			}

			printf ("\n");
		}


		reg_a=free_handle;
		esxdos_handler_no_error_uncarry();
		printf ("Successfully esxdos_handler_call_f_open file: %s\n",fullpath);


		if (stat(fullpath, &esxdos_fopen_files[free_handle].last_file_buf_stat)!=0) {
						printf("Unable to get status of file %s\n",fullpath);
		}

		//Indicar handle ocupado
		esxdos_fopen_files[free_handle].open_file.v=1;

		esxdos_fopen_files[free_handle].is_a_directory.v=0;
	}


	esxdos_handler_return_call();


}

void esxdos_handler_call_f_read(void)
{

	int file_handler=reg_a;

	if (file_handler>=ESXDOS_MAX_OPEN_FILES) {
		printf ("Error from esxdos_handler_call_f_read. Handler %d out of range\n",file_handler);
		esxdos_handler_error_carry(ESXDOS_ERROR_EBADF);
		esxdos_handler_return_call();
		return;
	}

	if (esxdos_fopen_files[file_handler].open_file.v==0) {
		printf ("Error from esxdos_handler_call_f_read. Handler %d not found\n",file_handler);
		esxdos_handler_error_carry(ESXDOS_ERROR_EBADF);
		esxdos_handler_return_call();
		return;
	}
	else {
		/*
		f_read                  equ fsys_base + 5;      // $9d  sbc a,l
;                                                                       // Read BC bytes at HL from file handle A.
;                                                                       // On return BC=number of bytes successfully
;                                                                       // read. File pointer is updated.
*/
		z80_int total_leidos=0;
		z80_int bytes_a_leer=reg_bc;
		int leidos=1;

		while (bytes_a_leer && leidos) {
			z80_byte byte_read;
			leidos=fread(&byte_read,1,1,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);
			if (leidos) {
					poke_byte_no_time(reg_hl+total_leidos,byte_read);
					total_leidos++;
					bytes_a_leer--;
			}
		}

		reg_bc=total_leidos;
		//reg_hl +=total_leidos; //???
		esxdos_handler_no_error_uncarry();

		printf ("Successfully esxdos_handler_call_f_read total bytes read: %d\n",total_leidos);

	}

	esxdos_handler_return_call();
}



void esxdos_handler_call_f_write(void)
{

	int file_handler=reg_a;

	if (file_handler>=ESXDOS_MAX_OPEN_FILES) {
		printf ("Error from esxdos_handler_call_f_write. Handler %d out of range\n",file_handler);
		esxdos_handler_error_carry(ESXDOS_ERROR_EBADF);
		esxdos_handler_return_call();
		return;
	}

	if (esxdos_fopen_files[file_handler].open_file.v==0) {
		printf ("Error from esxdos_handler_call_f_write. Handler %d not found\n",file_handler);
		esxdos_handler_error_carry(ESXDOS_ERROR_EBADF);
		esxdos_handler_return_call();
		return;
	}
	else {
		/*
		f_write                 equ fsys_base + 6;      // $9e  sbc a,(hl)
		;                                                                       // Write BC bytes from HL to file handle A.
		;                                                                       // On return BC=number of bytes successfully
		;                                                                       // written. File pointer is updated.
		*/

		if (esxdos_fopen_files[file_handler].tiene_plus3dos_header.v) {
			//Escribir primero cabecera PLUS3DOS
			//TODO: asumimos que se escriben todos los bytes de golpe->BC contiene la longitud total del bloque
			/*

Offset	Length	Description
0	8	"PLUS3DOS" identification text
8	1	0x1a marker byte (note 1)
9	1	Issue number   (01 en esxdos)
10	1	Version number (00 en esxdos)
11	4	Length of the file in bytes (note 2)
15..22		+3 Basic header data
23..126		Reserved (note 3)
127		Checksum (note 4))
*/
			char *cabe="PLUS3DOS";
			fwrite(cabe,1,8,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);

			z80_byte marker=0x1a,issue=1,version=1;
			fwrite(&marker,1,1,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);
			fwrite(&issue,1,1,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);
			fwrite(&version,1,1,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);

			//longitud
			fwrite(&reg_c,1,1,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);
			fwrite(&reg_b,1,1,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);
			z80_byte valor_nulo=0;
			fwrite(&valor_nulo,1,1,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);
			fwrite(&valor_nulo,1,1,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);

			//La cabecera que teniamos guardada
			int i;
			for (i=0;i<8;i++) {
				fwrite(&esxdos_fopen_files[file_handler].buffer_plus3dos_header[i],1,1,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);
			}

			//Reservado
			for (i=0;i<104;i++) fwrite(&valor_nulo,1,1,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);

			//Checksum. De momento inventarlo
			z80_byte valor_checksum=0;
			fwrite(&valor_checksum,1,1,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);
		}



		z80_int bytes_a_escribir=reg_bc;
		z80_byte byte_read;
		z80_int total_leidos=0;

		while (bytes_a_escribir) {
			byte_read=peek_byte_no_time(reg_hl+total_leidos);
			fwrite(&byte_read,1,1,esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);
			total_leidos++;
			bytes_a_escribir--;

		}

		esxdos_handler_no_error_uncarry();

		printf ("Successfully esxdos_handler_call_f_write total bytes write: %d\n",total_leidos);

	}

	esxdos_handler_return_call();
}

void esxdos_handler_call_f_close(void)
{

	//fclose tambien se puede llamar para cerrar lectura de directorio.

	int file_handler=reg_a;

	if (file_handler>=ESXDOS_MAX_OPEN_FILES) {
		printf ("Error from esxdos_handler_call_f_read. Handler %d out of range\n",file_handler);
		esxdos_handler_error_carry(ESXDOS_ERROR_EBADF);
		esxdos_handler_return_call();
		return;
	}



	if (esxdos_fopen_files[file_handler].open_file.v==0) {
		//printf ("Error from esxdos_handler_call_f_read. Handler %d not found\n",file_handler);
		esxdos_handler_error_carry(ESXDOS_ERROR_EBADF);
		esxdos_handler_return_call();
		return;

	}

	else {
		if (esxdos_fopen_files[file_handler].is_a_directory.v==0) {
			printf ("Closing a file\n");
			fclose(esxdos_fopen_files[file_handler].esxdos_last_open_file_handler_unix);
		}

		else {
			printf ("Closing a directory\n");
		}

		esxdos_fopen_files[file_handler].open_file.v=0;
		esxdos_handler_no_error_uncarry();
	}

	esxdos_handler_return_call();
}

//tener en cuenta raiz y directorio actual
//si localdir no es NULL, devolver directorio local (quitando esxdos_handler_root_dir)
//funcion igual a zxpand_get_final_directory, solo adaptando nombres de variables zxpand->esxdos_handler
void esxdos_handler_get_final_directory(char *dir, char *finaldir, char *localdir)
{


	//printf ("esxdos_handler_get_final_directory. dir: %s esxdos_handler_root_dir: %s\n",dir,esxdos_handler_root_dir);

	//Guardamos directorio actual del emulador
	char directorio_actual[PATH_MAX];
	getcwd(directorio_actual,PATH_MAX);

	//cambiar a directorio indicado, juntando raiz, dir actual de esxdos_handler, y dir
	char dir_pedido[PATH_MAX];

	//Si directorio pedido es absoluto, cambiar cwd
	if (dir[0]=='/') {
		sprintf (esxdos_handler_cwd,"%s",dir);
		sprintf (dir_pedido,"%s/%s",esxdos_handler_root_dir,esxdos_handler_cwd);
	}


	else {
		sprintf (dir_pedido,"%s/%s/%s",esxdos_handler_root_dir,esxdos_handler_cwd,dir);
	}

	menu_filesel_chdir(dir_pedido);

	//Ver en que directorio estamos
	//char dir_final[PATH_MAX];
	getcwd(finaldir,PATH_MAX);
	//printf ("total final directory: %s . esxdos_handler_root_dir: %s\n",finaldir,esxdos_handler_root_dir);
	//Esto suele retornar sim barra al final

	//Si finaldir no tiene barra al final, haremos que esxdos_handler_root_dir tampoco la tenga
	int l=strlen(finaldir);

	if (l) {
		if (finaldir[l-1]!='/' && finaldir[l-1]!='\\') {
			//finaldir no tiene barra al final
			int m=strlen(esxdos_handler_root_dir);
			if (m) {
				if (esxdos_handler_root_dir[m-1]=='/' || esxdos_handler_root_dir[m-1]=='\\') {
					//root dir tiene barra al final. quitarla
					//printf ("quitando barra del final de esxdos_handler_root_dir\n");
					esxdos_handler_root_dir[m-1]=0;
				}
			}
		}
	}


	//Ahora hay que quitar la parte del directorio raiz
	//printf ("running strstr (%s,%s)\n",finaldir,esxdos_handler_root_dir);
	char *s=strstr(finaldir,esxdos_handler_root_dir);

	if (s==NULL) {
		debug_printf (VERBOSE_DEBUG,"Directory change not allowed");
		//directorio final es el mismo que habia
		sprintf (finaldir,"%s",esxdos_handler_cwd);
		return;
	}

	//Si esta bien, meter parte local
	if (localdir!=NULL) {
		int l=strlen(esxdos_handler_root_dir);
		sprintf (localdir,"%s",&finaldir[l]);
		//printf ("local directory: %s\n",localdir);
	}

	//printf ("directorio final local de esxdos_handler: %s\n",finaldir);


	//Restauramos directorio actual del emulador
	menu_filesel_chdir(directorio_actual);
}


void esxdos_handler_call_f_chdir(void)
{

	char ruta[PATH_MAX];
	esxdos_handler_copy_hl_to_string(ruta);



		char directorio_final[PATH_MAX];

		printf ("Changing to directory %s\n",ruta);

		esxdos_handler_get_final_directory(ruta,directorio_final,esxdos_handler_cwd);

		printf ("Final directory %s . cwd: %s\n",directorio_final,esxdos_handler_cwd);


	esxdos_handler_no_error_uncarry();
	esxdos_handler_return_call();
}

void esxdos_handler_copy_string_to_hl(char *s)
{
	z80_int p=0;

	while (*s) {
		poke_byte_no_time(reg_hl+p,*s);
		s++;
		p++;
	}

	poke_byte_no_time(reg_hl+p,0);
}

void esxdos_handler_call_f_getcwd(void)
{

	// Get current folder path (null-terminated)
// to buffer. A=drive. HL=pointer to buffer.

	esxdos_handler_copy_string_to_hl(esxdos_handler_cwd);

	esxdos_handler_no_error_uncarry();
	esxdos_handler_return_call();
}

void esxdos_handler_call_f_opendir(void)
{

	/*

	;                                                                       // Open folder. A=drive. HL=Pointer to zero
;                                                                       // terminated string with path to folder.
;                                                                       // B=folder access mode. Only the BASIC
;                                                                       // header bit matters, whether you want to
;                                                                       // read header information or not. On return
;                                                                       // without error, A=folder handle.

*/

//Ver si no se han abierto el maximo de archivos y obtener handle libre
int free_handle=esxdos_find_free_fopen();
if (free_handle==-1) {
	esxdos_handler_error_carry(ESXDOS_ERROR_ENFILE);
	esxdos_handler_return_call();
	return;
}


	char directorio[PATH_MAX];

	esxdos_handler_copy_hl_to_string(directorio);
	printf ("opening directory %s\n",directorio);

	char directorio_final[PATH_MAX];
	//obtener directorio final
	esxdos_handler_get_final_directory((char *) directorio,directorio_final,NULL);


	//Guardar directorio final, hace falta al leer cada entrada para saber su tamanyo
	sprintf (esxdos_fopen_files[free_handle].esxdos_handler_last_dir_open,"%s",directorio_final);
	esxdos_fopen_files[free_handle].esxdos_handler_dfd = opendir(directorio_final);

	if (esxdos_fopen_files[free_handle].esxdos_handler_dfd == NULL) {
	 	printf("Can't open directory %s (full: %s)\n", directorio,directorio_final);
	  esxdos_handler_error_carry(ESXDOS_ERROR_ENOENT);
		esxdos_handler_return_call();
		return;
	}

	else {
		esxdos_fopen_files[free_handle].open_file.v=1;
		esxdos_fopen_files[free_handle].is_a_directory.v=1;
		reg_a=free_handle;
		esxdos_handler_no_error_uncarry();
	}


	esxdos_handler_return_call();
}

//comprobaciones de nombre de archivos en directorio
int esxdos_handler_readdir_no_valido(char *s)
{

	//Si longitud mayor que 12 (8 nombre, punto, 3 extension)
	//if (strlen(s)>12) return 0;

	printf ("checking is name %s is valid\n",s);


	char extension[NAME_MAX];
	char nombre[NAME_MAX];

	util_get_file_extension(s,extension);
	util_get_file_without_extension(s,nombre);

	//si nombre mayor que 8
	if (strlen(nombre)>8) return 0;

	//si extension mayor que 3
	if (strlen(extension)>3) return 0;


	//si hay letras minusculas
	//int i;
	//for (i=0;s[i];i++) {
	//	if (s[i]>='a' && s[i]<'z') return 0;
	//}



	return 1;

}

int esxdos_handler_string_to_msdos(char *fullname,z80_int puntero)
{
	z80_int i;

	//for (i=0;i<12;i++) poke_byte_no_time(puntero+i,' ');

	for (i=0;i<12 && fullname[i];i++) {
		poke_byte_no_time(puntero+i,fullname[i]);
	}

	poke_byte_no_time(puntero+i,0);
	i++;

	printf ("lenght name: %d\n",i);
	//if (i<11) poke_byte_no_time(puntero+i,0);

	//return 12;
	return i;

	//Primero rellenar con espacios




	//Aunque aqui no deberia exceder de 8 y 3, pero por si acaso
	char nombre[PATH_MAX];
	char extension[PATH_MAX];

	util_get_file_without_extension(fullname,nombre);
	util_get_file_extension(fullname,extension);

	printf ("name: %s extension: %s\n",nombre,extension);

	//Escribir nombre
	for (i=0;i<8 && nombre[i];i++) {
		printf ("%c",nombre[i]);
		poke_byte_no_time(puntero+i,nombre[i]);
	}

	printf (".");

	//Escribir extension
	for (i=0;i<3 && extension[i];i++) {
		printf ("%c",extension[i]);
		poke_byte_no_time(puntero+8+i,extension[i]);
	}

	printf ("\n");

}

void esxdos_handler_call_f_readdir(void)
{

	//Guardamos hl por si acaso, porque lo modificaremos para comodidad
	z80_int old_hl=reg_hl;

	int file_handler=reg_a;

	if (file_handler>=ESXDOS_MAX_OPEN_FILES) {
		printf ("Error from esxdos_handler_call_f_read. Handler %d out of range\n",file_handler);
		esxdos_handler_error_carry(ESXDOS_ERROR_EBADF);
		esxdos_handler_return_call();
		return;
	}

	/*
	f_readdir               equ fsys_base + 12;     // $a4  and h
;                                                                       // Read a folder entry to a buffer pointed
;                                                                       // to by HL. A=handle. Buffer format:
;                                                                       // <ASCII>  file/dirname
;                                                                       // <byte>   attributes (MS-DOS format)
;                                                                       // <dword>  date
;                                                                       // <dword>  filesize
;                                                                       // If opened with BASIC header bit, the
;                                                                       // BASIC header follows the normal entry
;                                                                       // (with type=$ff if headerless).
;                                                                       // On return, if A=1 there are more entries.
;                                                                       // If A=0 then it is the end of the folder.
;                                                                       // Does not currently return the size of an
;                                                                       // entry, or zero if end of folder reached.
*/

if (esxdos_fopen_files[file_handler].open_file.v==0) {
	printf ("Error from esxdos_handler_call_f_read. Handler %d not found\n",file_handler);
	esxdos_handler_error_carry(ESXDOS_ERROR_EBADF);
	esxdos_handler_return_call();
	return;
}

if (esxdos_fopen_files[file_handler].esxdos_handler_dfd==NULL) {
	esxdos_handler_error_carry(ESXDOS_ERROR_EBADF);
	esxdos_handler_return_call();
	return;
}

do {

	esxdos_fopen_files[file_handler].esxdos_handler_dp = readdir(esxdos_fopen_files[file_handler].esxdos_handler_dfd);

	if (esxdos_fopen_files[file_handler].esxdos_handler_dp == NULL) {
		closedir(esxdos_fopen_files[file_handler].esxdos_handler_dfd);
		esxdos_fopen_files[file_handler].esxdos_handler_dfd=NULL;
		//no hay mas archivos
		reg_a=0;
		esxdos_handler_no_error_uncarry();
		esxdos_handler_return_call();
		return;
	}


} while(!esxdos_handler_readdir_no_valido(esxdos_fopen_files[file_handler].esxdos_handler_dp->d_name));


//if (esxdos_handler_isValidFN(esxdos_handler_globaldata)

//meter flags
//esxdos_handler_filinfo_fattrib=0;



int longitud_nombre=strlen(esxdos_fopen_files[file_handler].esxdos_handler_dp->d_name);

//obtener nombre con directorio. obtener combinando directorio root, actual y inicio listado
char nombre_final[PATH_MAX];
util_get_complete_path(esxdos_fopen_files[file_handler].esxdos_handler_last_dir_open,esxdos_fopen_files[file_handler].esxdos_handler_dp->d_name,nombre_final);

z80_byte atributo_archivo=0;

/*
Attribute - a bitvector. Bit 0: read only. Bit 1: hidden.
        Bit 2: system file. Bit 3: volume label. Bit 4: subdirectory.
        Bit 5: archive. Bits 6-7: unused.

*/

//sprintf (nombre_final,"%s/%s",esxdos_handler_last_dir_open,esxdos_handler_dp->d_name);

//if (get_file_type(esxdos_handler_dp->d_type,esxdos_handler_dp->d_name)==2) {
if (get_file_type(esxdos_fopen_files[file_handler].esxdos_handler_dp->d_type,nombre_final)==2) {
	//meter flags directorio y nombre entre <>
	//esxdos_handler_filinfo_fattrib |=16;
	//sprintf((char *) &esxdos_handler_globaldata[0],"<%s>",esxdos_handler_dp->d_name);
	//longitud_nombre +=2;
	atributo_archivo |=16;
	printf ("Es directorio\n");
}

else {
	//sprintf((char *) &esxdos_handler_globaldata[0],"%s",esxdos_handler_dp->d_name);
}

//Meter nombre. Saltamos primer byte.
//poke_byte_no_time(reg_hl++,0);

/*
esxdos_handler_copy_string_to_hl(esxdos_handler_dp->d_name);

z80_int puntero=reg_hl+longitud_nombre+1; //saltar nombre+0 del final

*/

z80_int puntero=reg_hl;
//Atributos
poke_byte_no_time(puntero++,atributo_archivo);

int retornado_nombre=esxdos_handler_string_to_msdos(esxdos_fopen_files[file_handler].esxdos_handler_dp->d_name,puntero);

/*oke_byte_no_time(puntero++,'H');
poke_byte_no_time(puntero++,'O');
poke_byte_no_time(puntero++,'L');
poke_byte_no_time(puntero++,'A');
poke_byte_no_time(puntero++,' ');
poke_byte_no_time(puntero++,' ');
poke_byte_no_time(puntero++,' ');
poke_byte_no_time(puntero++,' ');

poke_byte_no_time(puntero++,'T');
poke_byte_no_time(puntero++,'X');
poke_byte_no_time(puntero++,'T');*/

//z80_int puntero=reg_hl+11;

puntero+=retornado_nombre;

/*
;                                                                       // <dword>  date
;                                                                       // <dword>  filesize
*/

//Atributos. TODO
//poke_byte_no_time(puntero++,0);

//Fecha. TODO
/*
22-23   Time (5/6/5 bits, for hour/minutes/doubleseconds)
24-25   Date (7/4/5 bits, for year-since-1980/month/day)
*/

int hora=11;
int minutos=15;
int doblesegundos=20*2;

int anyo=37;
int mes=9;
int dia=18;


get_file_date_from_name(nombre_final,&hora,&minutos,&doblesegundos,&dia,&mes,&anyo);

anyo-=1980;
doblesegundos *=2;

esxdos_handler_fill_date_struct(puntero,hora,minutos,doblesegundos,dia,mes,anyo);


//Tamanyo

//copia para ir dividiendo entre 256
long int longitud_total=get_file_size(nombre_final);

z80_long_int l=longitud_total;

printf ("lenght file: %d\n",l);
esxdos_handler_fill_size_struct(puntero+4,l);


//Dejamos hl como estaba por si acaso
reg_hl=old_hl;

reg_a=1; //Hay mas ficheros
	esxdos_handler_no_error_uncarry();
	esxdos_handler_return_call();

}

void esxdos_handler_call_f_fstat(void)
{

/*
f_fstat                 equ fsys_base + 9;      // $a1  and c
;                                                                       // Get file info/status to buffer at HL.
;                                                                       // A=handle. Buffer format:
;                                                                       // <byte>  drive
;                                                                       // <byte>  device
;                                                                       // <byte>  file attributes (MS-DOS format)
;                                                                       // <dword> date
;                                                                       // <dword> file size
*/
int file_handler=reg_a;

if (file_handler>=ESXDOS_MAX_OPEN_FILES) {
	printf ("Error from esxdos_handler_call_f_read. Handler %d out of range\n",file_handler);
	esxdos_handler_error_carry(ESXDOS_ERROR_EBADF);
	esxdos_handler_return_call();
	return;
}

if (esxdos_fopen_files[file_handler].open_file.v==0) {
	printf ("Error from esxdos_handler_call_f_read. Handler %d not found\n",file_handler);
	esxdos_handler_error_carry(ESXDOS_ERROR_EBADF);
	esxdos_handler_return_call();
	return;
}


	//TODO. segun file handler en A
	poke_byte_no_time(reg_hl,0); //drive
	poke_byte_no_time(reg_hl+1,0); //device


	z80_byte atributo_archivo=0;
	if (get_file_type_from_stat(&esxdos_fopen_files[file_handler].last_file_buf_stat)==2) {
		printf ("fstat: is a directory\n");
		atributo_archivo|=16;
	}

	poke_byte_no_time(reg_hl+2,atributo_archivo); //attrs

	//Fecha
	int hora=11;
	int minuto=15;
	int doblesegundos=20*2;

	int anyo=37;
	int mes=9;
	int dia=18;


	get_file_date_from_stat(&esxdos_fopen_files[file_handler].last_file_buf_stat,&hora,&minuto,&doblesegundos,&dia,&mes,&anyo);

	doblesegundos *=2;
	anyo -=1980;


	esxdos_handler_fill_date_struct(reg_hl+3,hora,minuto,doblesegundos,dia,mes,anyo);

	z80_long_int size=esxdos_fopen_files[file_handler].last_file_buf_stat.st_size;
	esxdos_handler_fill_size_struct(reg_hl+7,size);

	esxdos_handler_no_error_uncarry();
	esxdos_handler_return_call();

}

void esxdos_handler_run_normal_rst8(void)
{
	printf ("Running normal rst 8 call\n");
	rst(8);
}

void debug_rst8_esxdos(void)
{
	z80_byte funcion=peek_byte_no_time(reg_pc);

	char buffer_fichero[256];

	switch (funcion)
	{

		case ESXDOS_RST8_DISK_READ:
			printf ("ESXDOS_RST8_DISK_READ\n");
			esxdos_handler_run_normal_rst8();
		break;

		case ESXDOS_RST8_M_GETSETDRV:
			printf ("ESXDOS_RST8_M_GETSETDRV\n");
			esxdos_handler_run_normal_rst8();
	  break;

		case ESXDOS_RST8_F_OPEN:

			esxdos_handler_copy_hl_to_string(buffer_fichero);
			printf ("ESXDOS_RST8_F_OPEN. Mode: %02XH File: %s\n",reg_b,buffer_fichero);
			esxdos_handler_call_f_open();

		break;

		case ESXDOS_RST8_F_CLOSE:
			printf ("ESXDOS_RST8_F_CLOSE\n");
			esxdos_handler_call_f_close();

		break;

		case ESXDOS_RST8_F_READ:
		//Read BC bytes at HL from file handle A.
			printf ("ESXDOS_RST8_F_READ. Read %d bytes at %04XH from file handle %d\n",reg_bc,reg_hl,reg_a);
			esxdos_handler_call_f_read();
		break;

		case ESXDOS_RST8_F_WRITE:
		//Write BC bytes at HL from file handle A.
			printf ("ESXDOS_RST8_F_Write. Write %d bytes at %04XH from file handle %d\n",reg_bc,reg_hl,reg_a);
			esxdos_handler_call_f_write();
		break;

		case ESXDOS_RST8_F_GETCWD:
			printf ("ESXDOS_RST8_F_GETCWD\n");
			esxdos_handler_call_f_getcwd();
		break;

		case ESXDOS_RST8_F_CHDIR:
			esxdos_handler_copy_hl_to_string(buffer_fichero);
			printf ("ESXDOS_RST8_F_CHDIR: %s\n",buffer_fichero);
			esxdos_handler_call_f_chdir();
		break;

		case ESXDOS_RST8_F_OPENDIR:
			printf ("ESXDOS_RST8_F_OPENDIR\n");
			esxdos_handler_call_f_opendir();
		break;

		case ESXDOS_RST8_F_READDIR:
			printf ("ESXDOS_RST8_F_READDIR\n");
			esxdos_handler_call_f_readdir();
		break;

		case ESXDOS_RST8_F_FSTAT:
			printf ("ESXDOS_RST8_F_FSTAT\n");
			esxdos_handler_call_f_fstat();
		break;

		case 0xB3:
			printf ("Unknown ESXDOS_RST8 B3H. Return ok\n");
		//desconocida. salta cuando se hace un LOAD *"NOMBRE"
		//hace un fread con flags  FA_READ|FA_USE_HEADER  y luego llama a este 0xB3
			esxdos_handler_no_error_uncarry();
			esxdos_handler_return_call();
		break;



		default:
			if (funcion>=0x80) {
				printf ("Unhandled ESXDOS_RST8: %02XH !! \n",funcion);
				char buffer_registros[1024];
				print_registers(buffer_registros);
				printf ("%s\n",buffer_registros);

			}
			rst(8); //No queremos que muestre mensaje de debug
			//esxdos_handler_run_normal_rst8();
		break;
	}
}


void esxdos_handler_run(void)
{
	debug_rst8_esxdos();

}

void esxdos_handler_enable(void)
{

	//esto solo puede pasar activandolo por linea de comandos
	if (!MACHINE_IS_SPECTRUM) {
		debug_printf (VERBOSE_INFO,"ESXDOS handler can only be enabled on Spectrum");
		return;
	}

	//Si no esta diviface
	if (diviface_enabled.v==0) {
		debug_printf(VERBOSE_ERR,"ESXDOS handler needs divmmc or divide emulation");
	}

	debug_printf(VERBOSE_DEBUG,"Enabling ESXDOS handler");

	//root dir se pone directorio actual si esta vacio
	if (esxdos_handler_root_dir[0]==0) getcwd(esxdos_handler_root_dir,PATH_MAX);

	esxdos_handler_enabled.v=1;
	//directorio  vacio
	esxdos_handler_cwd[0]=0;

	//Inicializar array de archivos abiertos
	//esxdos_fopen_files[ESXDOS_MAX_OPEN_FILES];
	int i;
	for (i=0;i<ESXDOS_MAX_OPEN_FILES;i++) {
		esxdos_fopen_files[i].open_file.v=0;
	}
}



void esxdos_handler_disable(void)
{
	debug_printf(VERBOSE_DEBUG,"Disabling ESXDOS handler");
	esxdos_handler_enabled.v=0;
}
