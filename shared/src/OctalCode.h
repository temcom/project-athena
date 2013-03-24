//
//  OctalCode.h
//  hifi
//
//  Created by Stephen Birarda on 3/15/13.
//
//

#ifndef __hifi__OctalCode__
#define __hifi__OctalCode__

#include <string.h>

void printOctalCode(unsigned char * octalCode);
int bytesRequiredForCodeLength(unsigned char threeBitCodes);
bool isDirectParentOfChild(unsigned char *parentOctalCode, unsigned char * childOctalCode);
char branchIndexWithDescendant(unsigned char * ancestorOctalCode, unsigned char * descendantOctalCode);
unsigned char * childOctalCode(unsigned char * parentOctalCode, char childNumber);

#endif /* defined(__hifi__OctalCode__) */
