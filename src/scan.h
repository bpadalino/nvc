//
//  Copyright (C) 2022-2023  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#ifndef _SCAN_H
#define _SCAN_H

#include "prim.h"

typedef union {
   double  d;
   char   *s;
   int64_t n;
} yylval_t;

// Functions shared between VHDL and Verilog scanners

typedef enum { SOURCE_VHDL, SOURCE_VERILOG } hdl_kind_t;

void input_from_file(const char *file);
hdl_kind_t source_kind(void);
int processed_yylex(void);
const char *token_str(int tok);

// Private interface to Flex scanners

void begin_token(char *tok, int length);
int get_next_char(char *b, int max_buffer);

// Functions implemented by each scanner

void reset_vhdl_scanner(void);
void reset_verilog_scanner(void);

void reset_vhdl_parser(void);
void reset_verilog_parser(void);

// VHDL tokens
typedef enum {
   tEOF,
   tID,
   tENTITY,
   tIS,
   tEND,
   tGENERIC,
   tPORT,
   tCONSTANT,
   tCOMPONENT,
   tCONFIGURATION,
   tARCHITECTURE,
   tOF,
   tBEGIN,
   tFOR,
   tTYPE,
   tTO,
   tALL,
   tIN,
   tOUT,
   tBUFFER,
   tBUS,
   tUNAFFECTED,
   tSIGNAL,
   tDOWNTO,
   tPROCESS,
   tPOSTPONED,
   tWAIT,
   tREPORT,
   tLPAREN,
   tRPAREN,
   tSEMI,
   tASSIGN,
   tCOLON,
   tCOMMA,
   tINT,
   tSTRING,
   tERROR,
   tINOUT,
   tLINKAGE,
   tVARIABLE,
   tIF,
   tRANGE,
   tSUBTYPE,
   tUNITS,
   tPACKAGE,
   tLIBRARY,
   tUSE,
   tDOT,
   tNULL,
   tTICK,
   tFUNCTION,
   tIMPURE,
   tRETURN,
   tPURE,
   tARRAY,
   tBOX,
   tASSOC,
   tOTHERS,
   tASSERT,
   tSEVERITY,
   tON,
   tMAP,
   tTHEN,
   tELSE,
   tELSIF,
   tBODY,
   tWHILE,
   tLOOP,
   tAFTER,
   tALIAS,
   tATTRIBUTE,
   tPROCEDURE,
   tEXIT,
   tNEXT,
   tWHEN,
   tCASE,
   tLABEL,
   tGROUP,
   tLITERAL,
   tBAR,
   tLSQUARE,
   tRSQUARE,
   tINERTIAL,
   tTRANSPORT,
   tREJECT,
   tBITSTRING,
   tBLOCK,
   tWITH,
   tSELECT,
   tGENERATE,
   tACCESS,
   tFILE,
   tOPEN,
   tREAL,
   tUNTIL,
   tRECORD,
   tNEW,
   tSHARED,
   tAND,
   tOR,
   tNAND,
   tNOR,
   tXOR,
   tXNOR,
   tEQ,
   tNEQ,
   tLT,
   tLE,
   tGT,
   tGE,
   tPLUS,
   tMINUS,
   tAMP,
   tPOWER,
   tOVER,
   tSLL,
   tSRL,
   tSLA,
   tSRA,
   tROL,
   tROR,
   tMOD,
   tREM,
   tABS,
   tNOT,
   tTIMES,
   tGUARDED,
   tREVRANGE,
   tPROTECTED,
   tCONTEXT,
   tCONDIF,
   tCONDELSE,
   tCONDELSIF,
   tCONDEND,
   tCONDERROR,
   tCONDWARN,
   tSYNTHOFF,
   tSYNTHON,
   tMEQ,
   tMNEQ,
   tMLT,
   tMLE,
   tMGT,
   tMGE,
   tREGISTER,
   tDISCONNECT,
   tCCONV,
   tLTLT,
   tGTGT,
   tFORCE,
   tRELEASE,
   tCARET,
   tAT,
   tQUESTION,
   tPARAMETER,
   tCOVERAGEON,
   tCOVERAGEOFF,
} token_t;

#endif  // _SCAN_H
