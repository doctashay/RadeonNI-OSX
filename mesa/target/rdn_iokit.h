/* The wire ABI stays 32-bit; Leopard's LP64 entry points use 64-bit scalars. */
#ifndef RDN_IOKIT_H
#define RDN_IOKIT_H
#include <IOKit/IOKitLib.h>
#ifdef __LP64__
#include <stdarg.h>
static kern_return_t rdn_scalar(io_connect_t connection, uint32_t selector,
                               unsigned inputs, unsigned outputs, ...)
{
   uint64_t in[6], out[6];
   uint32_t *dest[6], count = outputs;
   va_list args;
   unsigned i;
   kern_return_t result;
   if (inputs > 6 || outputs > 6) return kIOReturnBadArgument;
   va_start(args, outputs);
   for (i = 0; i < inputs; i++) in[i] = va_arg(args, unsigned int);
   for (i = 0; i < outputs; i++) dest[i] = va_arg(args, uint32_t *);
   va_end(args);
   result = IOConnectCallScalarMethod(connection, selector, in, inputs, out, &count);
   if (!result && count != outputs) return kIOReturnUnderrun;
   if (!result)
      for (i = 0; i < outputs; i++) *dest[i] = (uint32_t)out[i];
   return result;
}
static kern_return_t rdn_structure(io_connect_t connection, uint32_t selector,
                                  unsigned inputs, IOByteCount *size, ...)
{
   uint64_t in[6];
   va_list args;
   unsigned i;
   void *out;
   size_t bytes = *size;
   kern_return_t result;
   if (inputs > 6) return kIOReturnBadArgument;
   va_start(args, size);
   for (i = 0; i < inputs; i++) in[i] = va_arg(args, unsigned int);
   out = va_arg(args, void *);
   va_end(args);
   result = IOConnectCallMethod(connection, selector, in, inputs, NULL, 0,
                                NULL, NULL, out, &bytes);
   *size = bytes;
   return result;
}
#define IOConnectMethodScalarIScalarO rdn_scalar
#define IOConnectMethodScalarIStructureO rdn_structure
#endif
#endif
