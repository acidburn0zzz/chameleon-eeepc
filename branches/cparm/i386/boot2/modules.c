/*
 * Copyright 2010 Evan Lojewski. All rights reserved.
 *
 */

#include "boot.h"
#include "bootstruct.h"
#include "multiboot.h"
#include "modules.h"

#ifndef DEBUG_MODULES
#define DEBUG_MODULES 0
#endif

#if DEBUG_MODULES
#define DBG(x...)	printf(x)
#else
#define DBG(x...)
#endif

// NOTE: Global so that modules can link with this
unsigned long long textAddress = 0;
unsigned long long textSection = 0;

moduleHook_t* moduleCallbacks = NULL;
moduleList_t* loadedModules = NULL;
symbolList_t* moduleSymbols = NULL;
unsigned int (*lookup_symbol)(const char*) = NULL;

#if DEBUG_MODULES
VOID print_hook_list()
{
	moduleHook_t* hooks = moduleCallbacks;
	printf("Hook list: \n");
	while(hooks)
	{
		printf("*  %s\n", hooks->name);
		hooks = hooks->next;
	}
	printf("\n");
}

VOID print_symbol_list()
{
	symbolList_t* symbol = moduleSymbols;
	printf("Symbol list: \n");
	while(symbol)
	{
		printf("*  %s\n", symbol->symbol);
		symbol = symbol->next;
	}
	printf("\n");
}
#endif


/*
 * Initialize the module system by loading the Symbols.dylib module.
 * Once loaded, locate the _lookup_symbol function so that internal
 * symbols can be resolved.
 */
EFI_STATUS init_module_system()
{
	msglog("* Attempting to load system module\n");
	
	// Intialize module system
    EFI_STATUS status = load_module(SYMBOLS_MODULE);
	if((status == EFI_SUCCESS) || (status == EFI_ALREADY_STARTED)/*should never happen*/ )        
	{
		lookup_symbol = (void*)lookup_all_symbols(SYMBOL_LOOKUP_SYMBOL);
		
		if((UInt32)lookup_symbol != 0xFFFFFFFF)
		{
			return status;
		}
		
	}
	
	return EFI_LOAD_ERROR;
}


/*
 * Load all modules in the /Extra/modules/ directory
 * Module depencdies will be loaded first
 * MOdules will only be loaded once. When loaded  a module must
 * setup apropriete function calls and hooks as required.
 * NOTE: To ensure a module loads after another you may 
 * link one module with the other. For dyld to allow this, you must
 * reference at least one symbol within the module.
 */

VOID load_all_modules()
{
	char* name;
	long flags;
	long time;
	struct dirstuff* moduleDir = opendir("/Extra/modules/");
	while(readdir(moduleDir, (const char**)&name, &flags, &time) >= 0)
	{
		if ((strcmp(SYMBOLS_MODULE,name)) == 0) continue; // if we found Symbols.dylib, just skip it

		if(strcmp(&name[strlen(name) - sizeof("dylib")], ".dylib") == 0)
		{
			char* tmp = malloc(strlen(name) + 1);
			strcpy(tmp, name);
			
			msglog("* Attempting to load module: %s\n", tmp);			
			if(load_module(tmp) != EFI_SUCCESS)
			{
				// failed to load or already loaded
				 free(tmp);
			}
		}
#if DEBUG_MODULES
		else 
		{
			DBG("Ignoring %s\n", name);
		}
#endif

	}
#if DEBUG_MODULES
	print_symbol_list();
#endif
}

/*
 * Load a module file in /Extra/modules
 * TODO: verify version number of module
 */
EFI_STATUS load_module(char* module)
{
	void (*module_start)(void) = NULL;

	
	// Check to see if the module has already been loaded
	if(is_module_loaded(module) == EFI_SUCCESS)
	{
		msglog("Module %s already registred\n", module);
		return EFI_ALREADY_STARTED;
	}
	
	char modString[128];
	int fh = -1;
	sprintf(modString, "/Extra/modules/%s", module);
	fh = open(modString);
	if(fh < 0)
	{		
#if DEBUG_MODULES
		DBG("Unable to locate module %s\n", modString);
		getc();
#else
		msglog("Unable to locate module %s\n", modString);
#endif
			return EFI_OUT_OF_RESOURCES;		
	}
	EFI_STATUS ret = EFI_SUCCESS;
	
	{
		int moduleSize = file_size(fh);
		char* module_base = (char*) malloc(moduleSize);
		if (moduleSize && read(fh, module_base, moduleSize) == moduleSize)
		{
			
			DBG("Module %s read in.\n", modString);
			
			// Module loaded into memory, parse it
			module_start = parse_mach(module_base, &load_module, &add_symbol);
			
			if(module_start && module_start != (void*)0xFFFFFFFF)
			{
				module_loaded(module/*moduleName, moduleVersion, moduleCompat*/);
				// Notify the system that it was laoded
				(*module_start)();	// Start the module
				msglog("%s successfully Loaded.\n", module);
			}
			else
			{
				// The module does not have a valid start function
				printf("Unable to start %s\n", module);	
				ret = EFI_NOT_STARTED;
#if DEBUG_MODULES			
				getc();
#endif
			}		
		}
		else
		{
			printf("Unable to read in module %s\n.", module);
#if DEBUG_MODULES		
			getc();
#endif
			ret = EFI_LOAD_ERROR;
		}
	}
	close(fh);
	return ret;
}

moduleHook_t* get_callback(const char* name)
{
	moduleHook_t* hooks = moduleCallbacks;
	
	// look for a hook. If it exists, return the moduleHook_t*,
	// If not, return NULL.
	while(hooks)
	{
		if(strcmp(name, hooks->name) == 0)
		{
			//DBG("Located hook %s\n", name);
			return hooks;
		}
		hooks = hooks->next;
	}
	return NULL;
	
}

/*
 *	execute_hook(  const char* name )
 *		name - Name of the module hook
 *			If any callbacks have been registered for this hook
 *			they will be executed now in the same order that the
 *			hooks were added.
 */
EFI_STATUS execute_hook(const char* name, void* arg1, void* arg2, void* arg3, void* arg4, void* arg5, void* arg6)
{
	DBG("Attempting to execute hook '%s'\n", name);
	moduleHook_t* hook = get_callback(name);
	
	if(hook)
	{
		// Loop through all callbacks for this module
		callbackList_t* callbacks = hook->callbacks;
		
		while(callbacks)
		{
			// Execute callback
			callbacks->callback(arg1, arg2, arg3, arg4, arg5, arg6);
			callbacks = callbacks->next;
		}
		DBG("Hook '%s' executed.\n", name); 
		return EFI_SUCCESS;
	}
	
	// Callback for this hook doesn't exist;
	DBG("No callbacks for '%s' hook.\n", name);
	return EFI_NOT_FOUND;
	
}

/*
 *	register_hook_callback(  const char* name,  void(*callback)())
 *		name - Name of the module hook to attach to.
 *		callbacks - The funciton pointer that will be called when the
 *			hook is executed. When registering a new callback name, the callback is added sorted.
 *			NOTE: the hooks take four void* arguments.
 */
VOID register_hook_callback(const char* name, void(*callback)(void*, void*, void*, void*, void*, void*))
{	
	DBG("Adding callback for '%s' hook.\n", name);
	
	moduleHook_t* hook = get_callback(name);
	
	if(hook)
	{
		// append
		callbackList_t* newCallback = malloc(sizeof(callbackList_t));
		newCallback->next = hook->callbacks;
		hook->callbacks = newCallback;
		newCallback->callback = callback;
	}
	else
	{
		// create new hook
		moduleHook_t* newHook = malloc(sizeof(moduleHook_t));		
		newHook->name = name;
		newHook->callbacks = malloc(sizeof(callbackList_t));
		newHook->callbacks->callback = callback;
		newHook->callbacks->next = NULL;
		
		newHook->next = moduleCallbacks;
		moduleCallbacks = newHook;
		
	}
	
#if DEBUG_MODULES
	print_hook_list();
	getc();
#endif
	
}

#if DEBUG_MODULES
unsigned long vmaddr;
long   vmsize;
#endif

/*
 * Parse through a macho module. The module will be rebased and binded
 * as specified in the macho header. If the module is successfully loaded
 * the module iinit address will be returned.
 * NOTE; all dependecies will be loaded before this module is started
 * NOTE: If the module is unable to load ot completeion, the modules
 * symbols will still be available (TODO: fix this). This should not
 * happen as all dependencies are verified before the sybols are read in.
 */
void* parse_mach(void* binary, EFI_STATUS(*dylib_loader)(char*), long long(*symbol_handler)(char*, long long, char))	// TODO: add param to specify valid archs
{	
	char is64 = false;
	void (*module_start)(void) = NULL;
	
	// Module info
	/*char* moduleName = NULL;
	UInt32 moduleVersion = 0;
	UInt32 moduleCompat = 0;
	*/
	
	// TODO convert all of the structs to a union	
	struct dyld_info_command* dyldInfoCommand = NULL;	
	struct symtab_command* symtabCommand = NULL;	

	{
		struct segment_command *segCommand = NULL;
		struct segment_command_64 *segCommand64 = NULL;
		struct dylib_command* dylibCommand = NULL;
		struct load_command *loadCommand = NULL;
		UInt32 binaryIndex = 0;
		UInt16 cmd = 0;
		
		// Parse through the load commands
		if(((struct mach_header*)binary)->magic == MH_MAGIC)
		{
			is64 = false;
			binaryIndex += sizeof(struct mach_header);
		}
		else if(((struct mach_header_64*)binary)->magic == MH_MAGIC_64)
		{
			// NOTE: modules cannot be 64bit...
			is64 = true;
			binaryIndex += sizeof(struct mach_header_64);
		}
		else
		{
			printf("Modules: Invalid mach magic\n");
			getc();
			return NULL;
		}
		
		
		
		/*if(((struct mach_header*)binary)->filetype != MH_DYLIB)
		 {
		 printf("Module is not a dylib. Unable to load.\n");
		 getc();
		 return NULL; // Module is in the incorrect format
		 }*/
		
		while(cmd < ((struct mach_header*)binary)->ncmds)
		{
			cmd++;
			
			loadCommand = binary + binaryIndex;
			UInt32 cmdSize = loadCommand->cmdsize;
			
			
			switch ((loadCommand->cmd & 0x7FFFFFFF))
			{
				case LC_SYMTAB:
					symtabCommand = binary + binaryIndex;
					break;
					
				case LC_SEGMENT: // 32bit macho
				{
					segCommand = binary + binaryIndex;
					
					//printf("Segment name is %s\n", segCommand->segname);
					
					if(strcmp("__TEXT", segCommand->segname) == 0)
					{
						UInt32 sectionIndex;
						
#if HARD_DEBUG_MODULES			
						unsigned long fileaddr;
						long   filesize;					
						vmaddr = (segCommand->vmaddr & 0x3fffffff);
						vmsize = segCommand->vmsize;	  
						fileaddr = ((unsigned long)(binary + binaryIndex) + segCommand->fileoff);
						filesize = segCommand->filesize;
						
						printf("segname: %s, vmaddr: %x, vmsize: %x, fileoff: %x, filesize: %x, nsects: %d, flags: %x.\n",
							   segCommand->segname, (unsigned)vmaddr, (unsigned)vmsize, (unsigned)fileaddr, (unsigned)filesize,
							   (unsigned) segCommand->nsects, (unsigned)segCommand->flags);
						getc();
#endif
						sectionIndex = sizeof(struct segment_command);
						
						struct section *sect;
						
						while(sectionIndex < segCommand->cmdsize)
						{
							sect = binary + binaryIndex + sectionIndex;
							
							sectionIndex += sizeof(struct section);
							
							
							if(strcmp("__text", sect->sectname) == 0)
							{
								// __TEXT,__text found, save the offset and address for when looking for the calls.
								textSection = sect->offset;
								textAddress = sect->addr;
								break;
							}					
						}
					}
					break;
				}				
				case LC_SEGMENT_64:	// 64bit macho's
				{
					segCommand64 = binary + binaryIndex;
					
					//printf("Segment name is %s\n", segCommand->segname);
					
					if(strcmp("__TEXT", segCommand64->segname) == 0)
					{
						UInt32 sectionIndex;
						
#if HARD_DEBUG_MODULES					
						
						unsigned long fileaddr;
						long   filesize;					
						vmaddr = (segCommand64->vmaddr & 0x3fffffff);
						vmsize = segCommand64->vmsize;	  
						fileaddr = ((unsigned long)(binary + binaryIndex) + segCommand64->fileoff);
						filesize = segCommand64->filesize;
						
						printf("segname: %s, vmaddr: %x, vmsize: %x, fileoff: %x, filesize: %x, nsects: %d, flags: %x.\n",
							   segCommand64->segname, (unsigned)vmaddr, (unsigned)vmsize, (unsigned)fileaddr, (unsigned)filesize,
							   (unsigned) segCommand64->nsects, (unsigned)segCommand64->flags);
						getc();
#endif
						
						sectionIndex = sizeof(struct segment_command_64);
						
						struct section_64 *sect;
						
						while(sectionIndex < segCommand64->cmdsize)
						{
							sect = binary + binaryIndex + sectionIndex;
							
							sectionIndex += sizeof(struct section_64);
							
							
							if(strcmp("__text", sect->sectname) == 0)
							{
								// __TEXT,__text found, save the offset and address for when looking for the calls.
								textSection = sect->offset;
								textAddress = sect->addr;
								
								break;
							}					
						}
					}
					
					break;
				}				
				case LC_DYSYMTAB:
					break;
					
				case LC_LOAD_DYLIB:
				case LC_LOAD_WEAK_DYLIB ^ LC_REQ_DYLD:
				{
					dylibCommand  = binary + binaryIndex;
					char* module  = binary + binaryIndex + ((UInt32)*((UInt32*)&dylibCommand->dylib.name));
					// TODO: verify version
					// =	dylibCommand->dylib.current_version;
					// =	dylibCommand->dylib.compatibility_version;
					char* name = malloc(strlen(module) + strlen(".dylib") + 1);
					sprintf(name, "%s.dylib", module);				
					if(dylib_loader == EFI_SUCCESS)
					{
						EFI_STATUS statue = dylib_loader(name);
						
						if( statue != EFI_SUCCESS)
						{
							free(name);
							if (statue != EFI_ALREADY_STARTED)
							{
								// Unable to load dependancy
								return NULL;
							}
							
						} 
						
					}
					break;
				}				
				case LC_ID_DYLIB:
					dylibCommand = binary + binaryIndex;
					/*moduleName =	binary + binaryIndex + ((UInt32)*((UInt32*)&dylibCommand->dylib.name));
					 moduleVersion =	dylibCommand->dylib.current_version;
					 moduleCompat =	dylibCommand->dylib.compatibility_version;
					 */
					break;
					
				case LC_DYLD_INFO:
					// Bind and rebase info is stored here
					dyldInfoCommand = binary + binaryIndex;
					break;
					
				case LC_UUID:
					break;
					
				case LC_UNIXTHREAD:
					break;
					
				default:
					DBG("Unhandled loadcommand 0x%X\n", loadCommand->cmd & 0x7FFFFFFF);
					break;
					
			}
			
			binaryIndex += cmdSize;
		}
		//if(!moduleName) return NULL;
	}		

	// bind_macho uses the symbols.
	module_start = (void*)handle_symtable((UInt32)binary, symtabCommand, symbol_handler, is64);

	// Rebase the module before binding it.
	if(dyldInfoCommand && dyldInfoCommand->rebase_off)
	{
		rebase_macho(binary, (char*)dyldInfoCommand->rebase_off, dyldInfoCommand->rebase_size);
	}
	
	if(dyldInfoCommand && dyldInfoCommand->bind_off)
	{
		bind_macho(binary, (char*)dyldInfoCommand->bind_off, dyldInfoCommand->bind_size);
	}
	
	if(dyldInfoCommand && dyldInfoCommand->weak_bind_off)
	{
		// NOTE: this currently should never happen.
		bind_macho(binary, (char*)dyldInfoCommand->weak_bind_off, dyldInfoCommand->weak_bind_size);
	}
	
	if(dyldInfoCommand && dyldInfoCommand->lazy_bind_off)
	{
		// NOTE: we are binding the lazy pointers as a module is laoded,
		// This should be changed to bind when a symbol is referened at runtime instead.
		bind_macho(binary, (char*)dyldInfoCommand->lazy_bind_off, dyldInfoCommand->lazy_bind_size);
	}

	return module_start;
	
}

// Based on code from dylibinfo.cpp and ImageLoaderMachOCompressed.cpp
void rebase_macho(void* base, char* rebase_stream, UInt32 size)
{
	rebase_stream += (UInt32)base;
	
	UInt8 immediate = 0;
	UInt8 opcode = 0;
	UInt8 type = 0;
	
	UInt32 segmentAddress = 0;
	
	
	
	UInt32 tmp  = 0;
	UInt32 tmp2  = 0;
	UInt8 bits = 0;
	UInt32 index = 0;
	
	int done = 0;
	unsigned int i = 0;
	
	while(/*!done &&*/ i < size)
	{
		immediate = rebase_stream[i] & REBASE_IMMEDIATE_MASK;
		opcode = rebase_stream[i] & REBASE_OPCODE_MASK;

		
		switch(opcode)
		{
			case REBASE_OPCODE_DONE:
				// Rebase complete.
				done = 1;
				break;
				
				
			case REBASE_OPCODE_SET_TYPE_IMM:
				// Set rebase type (pointer, absolute32, pcrel32)
				//DBG("Rebase type = 0x%X\n", immediate);
				type = immediate;
				break;
				
				
			case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
			{
				// Locate address to begin rebasing
				segmentAddress = 0;
				
				struct segment_command* segCommand = NULL; // NOTE: 32bit only
				
				{
					unsigned int binIndex = 0;
					index = 0;
					do
					{
						segCommand = base + sizeof(struct mach_header) +  binIndex;
						
						
						binIndex += segCommand->cmdsize;
						index++;
					}
					while(index <= immediate);
				}				
				
				segmentAddress = segCommand->fileoff;
				
				tmp = 0;
				bits = 0;
				do
				{
					tmp |= (rebase_stream[++i] & 0x7f) << bits;
					bits += 7;
				}
				while(rebase_stream[i] & 0x80);
				
				segmentAddress += tmp;
				break;
			}				
			case REBASE_OPCODE_ADD_ADDR_ULEB:
				// Add value to rebase address
				tmp = 0;
				bits = 0;
				do
				{
					tmp <<= bits;
					tmp |= rebase_stream[++i] & 0x7f;
					bits += 7;
				}
				while(rebase_stream[i] & 0x80);
				
				segmentAddress +=	tmp; 
				break;
				
			case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
				segmentAddress += immediate * sizeof(void*);
				break;
				
				
			case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
				index = 0;
				for (index = 0; index < immediate; ++index)
				{
					rebase_location(base + segmentAddress, (char*)base, type);
					segmentAddress += sizeof(void*);
				}
				break;
			
				
			case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
				tmp = 0;
				bits = 0;
				do
				{
					tmp |= (rebase_stream[++i] & 0x7f) << bits;
					bits += 7;
				}
				while(rebase_stream[i] & 0x80);
				
				index = 0;
				for (index = 0; index < tmp; ++index)
				{
					//DBG("\tRebasing 0x%X\n", segmentAddress);
					rebase_location(base + segmentAddress, (char*)base, type);					
					segmentAddress += sizeof(void*);
				}
				break;
				
			case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
				tmp = 0;
				bits = 0;
				do
				{
					tmp |= (rebase_stream[++i] & 0x7f) << bits;
					bits += 7;
				}
				while(rebase_stream[i] & 0x80);
				
				rebase_location(base + segmentAddress, (char*)base, type);
				
				segmentAddress += tmp + sizeof(void*);
				break;
				
			case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
				tmp = 0;
				bits = 0;
				do
				{
					tmp |= (rebase_stream[++i] & 0x7f) << bits;
					bits += 7;
				}
				while(rebase_stream[i] & 0x80);
				
				
				tmp2 =  0;
				bits = 0;
				do
				{
					tmp2 |= (rebase_stream[++i] & 0x7f) << bits;
					bits += 7;
				}
				while(rebase_stream[i] & 0x80);
				
				index = 0;
				for (index = 0; index < tmp; ++index)
				{

					rebase_location(base + segmentAddress, (char*)base, type);
					
					segmentAddress += tmp2 + sizeof(void*);
				}
				break;
			default:
				break;
		}
		i++;
	}
}

// Based on code from dylibinfo.cpp and ImageLoaderMachOCompressed.cpp
// NOTE: this uses 32bit values, and not 64bit values. 
// There is apossibility that this could cause issues,
// however the macho file is 32 bit, so it shouldn't matter too much
void bind_macho(void* base, char* bind_stream, UInt32 size)
{	
	bind_stream += (UInt32)base;
	
	UInt8 immediate = 0;
	UInt8 opcode = 0;
	UInt8 type = 0;
	
	UInt32 segmentAddress = 0;
	
	UInt32 address = 0;
	
	SInt32 addend = 0;			// TODO: handle this
	SInt32 libraryOrdinal = 0;

	const char* symbolName = NULL;
	UInt8 symboFlags = 0;
	UInt32 symbolAddr = 0xFFFFFFFF;
	
	// Temperary variables
	UInt8 bits = 0;
	UInt32 tmp = 0;
	UInt32 tmp2 = 0;
	
	UInt32 index = 0;
	int done = 0;
	unsigned int i = 0;
	
	while(/*!done &&*/ i < size)
	{
		immediate = bind_stream[i] & BIND_IMMEDIATE_MASK;
		opcode = bind_stream[i] & BIND_OPCODE_MASK;
		
		
		switch(opcode)
		{
			case BIND_OPCODE_DONE:
				done = 1; 
				break;
				
			case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
				libraryOrdinal = immediate;
				//DBG("BIND_OPCODE_SET_DYLIB_ORDINAL_IMM: %d\n", libraryOrdinal);
				break;
				
			case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
				libraryOrdinal = 0;
				bits = 0;
				do
				{
					libraryOrdinal |= (bind_stream[++i] & 0x7f) << bits;
					bits += 7;
				}
				while(bind_stream[i] & 0x80);
				
				//DBG("BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: %d\n", libraryOrdinal);

				break;
				
			case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
				// NOTE: this is wrong, fortunately we don't use it
				libraryOrdinal = -immediate;
				//DBG("BIND_OPCODE_SET_DYLIB_SPECIAL_IMM: %d\n", libraryOrdinal);

				break;
				
			case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
				symboFlags = immediate;
				symbolName = (char*)&bind_stream[++i];
				i += strlen((char*)&bind_stream[i]);
				//DBG("BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: %s, 0x%X\n", symbolName, symboFlags);

				symbolAddr = lookup_all_symbols(symbolName);

				break;
				
			case BIND_OPCODE_SET_TYPE_IMM:
				// Set bind type (pointer, absolute32, pcrel32)
				type = immediate;
				//DBG("BIND_OPCODE_SET_TYPE_IMM: %d\n", type);

				break;
				
			case BIND_OPCODE_SET_ADDEND_SLEB:
				addend = 0;
				bits = 0;
				do
				{
					addend |= (bind_stream[++i] & 0x7f) << bits;
					bits += 7;
				}
				while(bind_stream[i] & 0x80);
				
				if(!(bind_stream[i-1] & 0x40)) addend *= -1;
				
				//DBG("BIND_OPCODE_SET_ADDEND_SLEB: %d\n", addend);
				break;
				
			case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
			{
				segmentAddress = 0;
				
				// Locate address
				struct segment_command* segCommand = NULL;	// NOTE: 32bit only
				
				{
					unsigned int binIndex = 0;
					index = 0;
					do
					{
						segCommand = base + sizeof(struct mach_header) +  binIndex;
						binIndex += segCommand->cmdsize;
						index++;
					}while(index <= immediate);
				}
				
				
				segmentAddress = segCommand->fileoff;
				
				// Read in offset
				tmp  = 0;
				bits = 0;
				do
				{
					tmp |= (bind_stream[++i] & 0x7f) << bits;
					bits += 7;
				}while(bind_stream[i] & 0x80);
				
				segmentAddress += tmp;
				
				//DBG("BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: 0x%X\n", segmentAddress);
				break;
			}				
			case BIND_OPCODE_ADD_ADDR_ULEB:
				// Read in offset
				tmp  = 0;
				bits = 0;
				do
				{
					tmp |= (bind_stream[++i] & 0x7f) << bits;
					bits += 7;
				}
				while(bind_stream[i] & 0x80);
				
				segmentAddress += tmp;
				//DBG("BIND_OPCODE_ADD_ADDR_ULEB: 0x%X\n", segmentAddress);
				break;
				
			case BIND_OPCODE_DO_BIND:
				//DBG("BIND_OPCODE_DO_BIND\n");
				if(symbolAddr != 0xFFFFFFFF)
				{
					address = segmentAddress + (UInt32)base;

					bind_location((UInt32*)address, (char*)symbolAddr, addend, BIND_TYPE_POINTER);
				}
				else if(strcmp(symbolName, SYMBOL_DYLD_STUB_BINDER) != 0)
				{
					printf("Unable to bind symbol %s\n", symbolName);
				}
				
				segmentAddress += sizeof(void*);
				break;
				
			case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
				//DBG("BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB\n");
				
				
				// Read in offset
				tmp  = 0;
				bits = 0;
				do
				{
					tmp |= (bind_stream[++i] & 0x7f) << bits;
					bits += 7;
				}
				while(bind_stream[i] & 0x80);
				
				
				
				if(symbolAddr != 0xFFFFFFFF)
				{
					address = segmentAddress + (UInt32)base;

					bind_location((UInt32*)address, (char*)symbolAddr, addend, BIND_TYPE_POINTER);
				}
				else if(strcmp(symbolName, SYMBOL_DYLD_STUB_BINDER) != 0)
				{
					printf("Unable to bind symbol %s\n", symbolName);
				}
				segmentAddress += tmp + sizeof(void*);

				
				break;
				
			case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
				//DBG("BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED\n");
				
				if(symbolAddr != 0xFFFFFFFF)
				{
					address = segmentAddress + (UInt32)base;

					bind_location((UInt32*)address, (char*)symbolAddr, addend, BIND_TYPE_POINTER);
				}
				else if(strcmp(symbolName, SYMBOL_DYLD_STUB_BINDER) != 0)
				{
					printf("Unable to bind symbol %s\n", symbolName);
				}
				segmentAddress += (immediate * sizeof(void*)) + sizeof(void*);

				
				break;
				
			case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:

				tmp  = 0;
				bits = 0;
				do
				{
					tmp |= (bind_stream[++i] & 0x7f) << bits;
					bits += 7;
				}
				while(bind_stream[i] & 0x80);

				
				tmp2  = 0;
				bits = 0;
				do
				{
					tmp2 |= (bind_stream[++i] & 0x7f) << bits;
					bits += 7;
				}
				while(bind_stream[i] & 0x80);
				
				
				//DBG("BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB 0x%X 0x%X\n", tmp, tmp2);

				
				if(symbolAddr != 0xFFFFFFFF)
				{
					for(index = 0; index < tmp; index++)
					{
						
						address = segmentAddress + (UInt32)base;

						bind_location((UInt32*)address, (char*)symbolAddr, addend, BIND_TYPE_POINTER);
						
						segmentAddress += tmp2 + sizeof(void*);
					}
				}
				else if(strcmp(symbolName, SYMBOL_DYLD_STUB_BINDER) != 0)
				{
					printf("Unable to bind symbol %s\n", symbolName);
				}
				
				
				break;
			default:
				break;
				
		}
		i++;
	}
}

inline void rebase_location(UInt32* location, char* base, int type)
{	
	switch(type)
	{
		case REBASE_TYPE_POINTER:
		case REBASE_TYPE_TEXT_ABSOLUTE32:
			*location += (UInt32)base;
			break;
			
		default:
			break;
	}
}

inline void bind_location(UInt32* location, char* value, UInt32 addend, int type)
{	
	// do actual update
	char* newValue = value + addend;

	switch (type) {
		case BIND_TYPE_POINTER:
		case BIND_TYPE_TEXT_ABSOLUTE32:
			break;

		case BIND_TYPE_TEXT_PCREL32:
			newValue -=  ((UInt32)location + 4);

			break;
		default:
			return;
	}
	*location = (UInt32)newValue;
	

}

/*
 * add_symbol
 * This function adds a symbol from a module to the list of known symbols 
 * possibly change to a pointer and add this to the Symbol module so that it can
 * adjust it's internal symbol list (sort) to optimize locating new symbols
 * NOTE: returns the address if the symbol is "start", else returns 0xFFFFFFFF
 */
long long add_symbol(char* symbol, long long addr, char is64)
{
	if(is64) return  0xFFFFFFFF; // Fixme
	
	// This only can handle 32bit symbols 
	symbolList_t* new_entry= malloc(sizeof(symbolList_t));
	DBG("Adding symbol %s at 0x%X\n", symbol, addr);
	if (new_entry)
	{	
		new_entry->next = moduleSymbols;
		
		moduleSymbols = new_entry;
		
		new_entry->addr = (UInt32)addr;
		new_entry->symbol = symbol;
		return addr;
	}	
	
	return 0xFFFFFFFF;
	
}

/*
 * print out the information about the loaded module
 */
VOID module_loaded(const char* name/*, UInt32 version, UInt32 compat*/)
{
	moduleList_t* new_entry = malloc(sizeof(moduleList_t));
	if (new_entry)
	{	
		new_entry->next = loadedModules;
	
		loadedModules = new_entry;
	
		new_entry->module = (char*)name;
		//	new_entry->version = version;
		//	new_entry->compat = compat;
	}
}

EFI_STATUS is_module_loaded(const char* name)
{
	moduleList_t* entry = loadedModules;
	while(entry)
	{
		DBG("Comparing %s with %s\n", name, entry->module);
		char fullname[128];
		sprintf(fullname, "%s.dylib",name);
		if((strcmp(entry->module, name) == 0) || (strcmp(entry->module, fullname) == 0))
		{
			DBG("Located module %s\n", name);
			return EFI_SUCCESS;
		}
		else
		{
			entry = entry->next;
		}

	}
	DBG("Module %s not found\n", name);

	return EFI_NOT_FOUND;
}

// Look for symbols using the Smbols moduel function.
// If non are found, look through the list of module symbols
unsigned int lookup_all_symbols(const char* name)
{
	{
		unsigned int addr = 0xFFFFFFFF;
		if(lookup_symbol && (UInt32)lookup_symbol != 0xFFFFFFFF)
		{
			addr = lookup_symbol(name);
			if(addr != 0xFFFFFFFF)
			{
				DBG("Internal symbol %s located at 0x%X\n", name, addr);
				return addr;
			}
		}
	}	

	{
		symbolList_t* entry = moduleSymbols;
		while(entry)
		{
			if(strcmp(entry->symbol, name) == 0)
			{
				DBG("External symbol %s located at 0x%X\n", name, entry->addr);
				return entry->addr;
			}
			else
			{
				entry = entry->next;
			}
			
		}
	}
	
#if DEBUG_MODULES
	if(strcmp(name, SYMBOL_DYLD_STUB_BINDER) != 0)
	{
		verbose("Unable to locate symbol %s\n", name);
		getc();
	}
#endif
	return 0xFFFFFFFF;
}


/*
 * parse the symbol table
 * Lookup any undefined symbols
 */
 
unsigned int handle_symtable(UInt32 base, struct symtab_command* symtabCommand, long long(*symbol_handler)(char*, long long, char), char is64)
{		
	unsigned int module_start = 0xFFFFFFFF;
	
	UInt32 symbolIndex = 0;
	char* symbolString = base + (char*)symtabCommand->stroff;
	//char* symbolTable = base + symtabCommand->symoff;
	if(!is64)
	{
		struct nlist* symbolEntry = (void*)base + symtabCommand->symoff;
		while(symbolIndex < symtabCommand->nsyms)
		{						
			if(symbolEntry->n_value)
			{				
				if(strcmp(symbolString + symbolEntry->n_un.n_strx, "start") == 0)
				{
					// Module start located. 'start' is an alias so don't register it				
					module_start = base + symbolEntry->n_value;
					DBG("n_value %x module_start %x\n", (unsigned)symbolEntry->n_value, (unsigned)module_start);
				}
				else
				{
					symbol_handler(symbolString + symbolEntry->n_un.n_strx, (long long)base + symbolEntry->n_value, is64);
				}
				
#if HARD_DEBUG_MODULES	
				bool isTexT = (((unsigned)symbolEntry->n_value > (unsigned)vmaddr) && ((unsigned)(vmaddr + vmsize) > (unsigned)symbolEntry->n_value ));
				printf("%s %s\n", isTexT ? "__TEXT :" : "__DATA(OR ANY) :", symbolString + symbolEntry->n_un.n_strx);
				
				if(strcmp(symbolString + symbolEntry->n_un.n_strx, "_BootHelp_txt") == 0)
				{
					long long addr = (long long)base + symbolEntry->n_value;
					unsigned char *BootHelp = NULL;
					BootHelp  = (unsigned char*)(UInt32)addr;					
					printf("method 1: __DATA : BootHelp_txt[0] %x\n", BootHelp[0]);
					
					long long addr2 = symbolEntry->n_value;
					unsigned char *BootHelp2 = NULL;
					BootHelp2  = (unsigned char*)(UInt32)addr2;					
					printf("method 2:  __DATA : BootHelp_txt[0] %x\n", BootHelp2[0]);
				}
#endif
			}
			
			symbolEntry++;
			symbolIndex++;	// TODO remove
		}
	}
	else
	{
		struct nlist_64* symbolEntry = (void*)base + symtabCommand->symoff;
		// NOTE First entry is *not* correct, but we can ignore it (i'm getting radar:// right now)	
		while(symbolIndex < symtabCommand->nsyms)
		{	
			
			if(strcmp(symbolString + symbolEntry->n_un.n_strx, "start") == 0)
			{
				// Module start located. 'start' is an alias so don't register it				
				module_start = base + symbolEntry->n_value;
			}
			else
			{
				symbol_handler(symbolString + symbolEntry->n_un.n_strx, (long long)base + symbolEntry->n_value, is64);
			}
			
			symbolEntry++;
			symbolIndex++;	// TODO remove
		}
	}
		
	return module_start;
	
}


/*
 * Locate the symbol for an already loaded function and modify the beginning of
 * the function to jump directly to the new one
 * example: replace_function("_HelloWorld_start", &replacement_start);
 */
EFI_STATUS replace_function(const char* symbol, void* newAddress)
{		 
	// TODO: look into using the next four bytes of the function instead
	// Most functions should support this, as they probably will be at 
	// least 10 bytes long, but you never know, this is sligtly safer as
	// function can be as small as 6 bytes.
	UInt32 addr = lookup_all_symbols(symbol);
	
	char* binary = (char*)addr;
	if(addr != 0xFFFFFFFF)
	{
		UInt32* jumpPointer = malloc(sizeof(UInt32*));
		
		*binary++ = 0xFF;	// Jump
		*binary++ = 0x25;	// Long Jump
		*((UInt32*)binary) = (UInt32)jumpPointer;
		
		*jumpPointer = (UInt32)newAddress;
		
		return EFI_SUCCESS;
	}
	
	return EFI_NOT_FOUND;

}


/* Nedded to divide 64bit numbers correctly. TODO: look into why modules need this
 * And why it isn't needed when compiled into boot2
 *
 * In the next versions, this will be surely replaced by the Apple's libcc_kext or the meklort's klibc
 */

uint64_t __udivdi3(uint64_t numerator, uint64_t denominator)
{
	uint64_t quotient = 0, qbit = 1;
	
	if (denominator)
	{
		while ((int64_t) denominator >= 0)
		{
			denominator <<= 1;
			qbit <<= 1;
		}
		
		while (denominator)
		{
			if (denominator <= numerator)
			{
				numerator -= denominator;
				quotient += qbit;
			}
			denominator >>= 1;
			qbit >>= 1;
		}
		
		return quotient;
	}
	
	stop("Divide by 0");
	return 0;	
}
