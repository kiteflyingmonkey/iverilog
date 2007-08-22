/*
 * Copyright (c) 2001 Stephen Williams (steve@icarus.com)
 *
 *    This source code is free software; you can redistribute it
 *    and/or modify it in source code form under the terms of the GNU
 *    General Public License as published by the Free Software
 *    Foundation; either version 2 of the License, or (at your option)
 *    any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */
#ifdef HAVE_CVS_IDENT
#ident "$Id: vpi_priv.cc,v 1.54 2007/04/18 01:57:07 steve Exp $"
#endif

# include  "vpi_priv.h"
# include  "schedule.h"
# include  <stdio.h>
# include  <stdarg.h>
# include  <string.h>
# include  <assert.h>
#ifdef HAVE_MALLOC_H
# include  <malloc.h>
#endif
# include  <stdlib.h>
# include  <math.h>

vpi_mode_t vpi_mode_flag = VPI_MODE_NONE;
FILE*vpi_trace = 0;

static s_vpi_vlog_info  vpi_vlog_info;
static s_vpi_error_info vpip_last_error = { 0, 0, 0, 0, 0, 0, 0 };

/*
 * The vpip_string function creates a constant string from the pass
 * input. This constant string is permanently allocate from an
 * efficient string buffer store.
 */
struct vpip_string_chunk {
      struct vpip_string_chunk*next;
      char data[64*1024 - sizeof (struct vpip_string_chunk*)];
};

const char *vpip_string(const char*str)
{
      static vpip_string_chunk first_chunk = {0, {0}};
      static vpip_string_chunk*chunk_list = &first_chunk;
      static unsigned chunk_fill = 0;

      unsigned len = strlen(str);
      assert( (len+1) <= sizeof chunk_list->data );

      if ( (len+1) > (sizeof chunk_list->data - chunk_fill) ) {
	    vpip_string_chunk*tmp = new vpip_string_chunk;
	    tmp->next = chunk_list;
	    chunk_list = tmp;
	    chunk_fill = 0;
      }

      char*res = chunk_list->data + chunk_fill;
      chunk_fill += len + 1;

      strcpy(res, str);
      return res;
}

static unsigned hash_string(const char*text)
{
      unsigned h = 0;

      while (*text) {
	    h = (h << 4) ^ (h >> 28) ^ *text;
	    text += 1;
      }
      return h;
}

const char* vpip_name_string(const char*text)
{
      const unsigned HASH_SIZE = 4096;
      static const char*hash_table[HASH_SIZE] = {0};

      unsigned hash_value = hash_string(text) % HASH_SIZE;

	/* If we easily find the string in the hash table, then return
	   that and be done. */
      if (hash_table[hash_value]
	  && (strcmp(hash_table[hash_value], text) == 0)) {
	    return hash_table[hash_value];
      }

	/* The existing hash entry is not a match. Replace it with the
	   newly allocated value, and return the new pointer as the
	   result to the add. */
      const char*res = vpip_string(text);
      hash_table[hash_value] = res;

      return res;
}
PLI_INT32 vpi_chk_error(p_vpi_error_info info)
{
      if (vpip_last_error.state == 0)
	    return 0;

      info->state = vpip_last_error.state;
      info->level = vpip_last_error.level;
      info->message = vpip_last_error.message;
      info->product = vpi_vlog_info.product;
      info->code = "";
      info->file = 0;
      info->line = 0;

      return info->level;
}

/*
 * When a task is called, this value is set so that vpi_handle can
 * fathom the vpi_handle(vpiSysTfCall,0) function.
 */
struct __vpiSysTaskCall*vpip_cur_task = 0;

PLI_INT32 vpi_free_object(vpiHandle ref)
{
      int rtn;

      if (vpi_trace) {
	    fprintf(vpi_trace, "vpi_free_object(%p)", ref);
	    fflush(vpi_trace);
      }

      assert(ref);
      if (ref->vpi_type->vpi_free_object_ == 0)
	    rtn = 1;
      else
	    rtn = ref->vpi_type->vpi_free_object_(ref);

      if (vpi_trace)
	    fprintf(vpi_trace, " --> %d\n", rtn);

      return rtn;
}

static int vpip_get_global(int property)
{
      switch (property) {

	  case vpiTimePrecision:
	    return vpip_get_time_precision();

	  default:
	    fprintf(stderr, "vpi error: bad global property: %d\n", property);
	    assert(0);
	    return vpiUndefined;
      }
}

static const char* vpi_property_str(PLI_INT32 code)
{
      static char buf[32];
      switch (code) {
	  case vpiConstType:
	    return "vpiConstType";
	  case vpiName:
	    return "vpiName";
	  case vpiFullName:
	    return "vpiFullName";
	  case vpiTimeUnit:
	    return "vpiTimeUnit";
	  default:
	    sprintf(buf, "%d", code);
      }
      return buf;
}

static const char* vpi_type_values(PLI_INT32 code)
{
      static char buf[32];
      switch (code) {
	  case vpiConstant:
	    return "vpiConstant";
	  case vpiIntegerVar:
	    return "vpiIntegerVar";
	  case vpiFunction:
	    return "vpiFunction";
	  case vpiModule:
	    return "vpiModule";
	  case vpiNet:
	    return "vpiNet";
	  case vpiParameter:
	    return "vpiParameter";
	  case vpiRealVar:
	    return "vpiRealVar";
	  case vpiReg:
	    return "vpiReg";
	  case vpiTask:
	    return "vpiTask";
	  default:
	    sprintf(buf, "%d", code);
      }
      return buf;
}

PLI_INT32 vpi_get(int property, vpiHandle ref)
{
      if (ref == 0)
	    return vpip_get_global(property);

      if (property == vpiType) {
	    if (vpi_trace) {
		  fprintf(vpi_trace, "vpi_get(vpiType, %p) --> %s\n",
			  ref, vpi_type_values(ref->vpi_type->type_code));
	    }

	    struct __vpiSignal*rfp = (struct __vpiSignal*)ref;
	    if (ref->vpi_type->type_code == vpiReg && rfp->isint_)
		  return vpiIntegerVar;
	    else
		  return ref->vpi_type->type_code;
      }

      if (ref->vpi_type->vpi_get_ == 0) {
	    if (vpi_trace) {
		  fprintf(vpi_trace, "vpi_get(%s, %p) --X\n",
			  vpi_property_str(property), ref);
	    }

	    return vpiUndefined;
      }

      int res = (ref->vpi_type->vpi_get_)(property, ref);

      if (vpi_trace) {
	    fprintf(vpi_trace, "vpi_get(%s, %p) --> %d\n",
		    vpi_property_str(property), ref, res);
      }

      return res;
}

char* vpi_get_str(PLI_INT32 property, vpiHandle ref)
{
      if (ref == 0) {
	    fprintf(stderr, "vpi error: vpi_get_str(%s, 0) called "
		    "with null vpiHandle.\n", vpi_property_str(property));
	    return 0;
      }

      if (property == vpiType) {
	    if (vpi_trace) {
		  fprintf(vpi_trace, "vpi_get(vpiType, %p) --> %s\n",
			  ref, vpi_type_values(ref->vpi_type->type_code));
	    }

	    struct __vpiSignal*rfp = (struct __vpiSignal*)ref;
            PLI_INT32 type;
	    if (ref->vpi_type->type_code == vpiReg && rfp->isint_)
		  type = vpiIntegerVar;
	    else
		  type = ref->vpi_type->type_code;
	    return (char *)vpi_type_values(type);
      }

      if (ref->vpi_type->vpi_get_str_ == 0) {
	    if (vpi_trace) {
		  fprintf(vpi_trace, "vpi_get_str(%s, %p) --X\n",
			  vpi_property_str(property), ref);
	    }
	    return 0;
      }

      char*res = (char*)(ref->vpi_type->vpi_get_str_)(property, ref);

      if (vpi_trace) {
	    fprintf(vpi_trace, "vpi_get_str(%s, %p) --> %s\n",
		    vpi_property_str(property), ref, res);
      }

      return res;
}

int vpip_time_units_from_handle(vpiHandle obj)
{
      struct __vpiSysTaskCall*task;
      struct __vpiScope*scope;
      struct __vpiSignal*signal;

      if (obj == 0)
	    return vpip_get_time_precision();

      switch (obj->vpi_type->type_code) {
	  case vpiSysTaskCall:
	    task = (struct __vpiSysTaskCall*)obj;
	    return task->scope->time_units;

	  case vpiModule:
	    scope = (struct __vpiScope*)obj;
	    return scope->time_units;

	  case vpiNet:
	  case vpiReg:
	    signal = (struct __vpiSignal*)obj;
 	    return signal->scope->time_units;

	  default:
	    fprintf(stderr, "ERROR: vpi_get_time called with object "
		    "handle type=%u\n", obj->vpi_type->type_code);
	    assert(0);
	    return 0;
      }
}

void vpi_get_time(vpiHandle obj, s_vpi_time*vp)
{
      int units;
      vvp_time64_t time;

      assert(vp);

      time = schedule_simtime();

      switch (vp->type) {
          case vpiSimTime:
	    vp->high = (time >> 32) & 0xffffffff;
	    vp->low = time & 0xffffffff;
	    break;

          case vpiScaledRealTime:
	    units = vpip_time_units_from_handle(obj);
            vp->real = pow(10.0L, vpip_get_time_precision() - units);
            vp->real *= time;
	    break;

          default:
            fprintf(stderr, "vpi_get_time: unknown type: %d\n", vp->type);
            assert(0);
	    break;
      }
}

PLI_INT32 vpi_get_vlog_info(p_vpi_vlog_info vlog_info_p)
{
    if (vlog_info_p != 0) {
	  *vlog_info_p = vpi_vlog_info;
	  return 1;
    } else {
	  return 0;
    }
}

void vpi_set_vlog_info(int argc, char** argv)
{
    vpi_vlog_info.product = "Icarus Verilog";
    vpi_vlog_info.version = "$Name:  $";
    vpi_vlog_info.argc    = argc;
    vpi_vlog_info.argv    = argv;

    static char trace_buf[1024];

    if (const char*path = getenv("VPI_TRACE")) {
	  if (!strcmp(path,"-"))
		vpi_trace = stdout;
	  else {
		vpi_trace = fopen(path, "w");
		if (!vpi_trace) {
		      perror(path);
		      exit(1);
		}
		setvbuf(vpi_trace, trace_buf, _IOLBF, sizeof(trace_buf));
	  }
    }
}

/*
 * This is a generic function to convert a vvp_vector4_t value into a
 * vpi_value structure. The format is selected by the format of the
 * value pointer. The width is the real width of the word, in case the
 * word_val width is not accurate.
 */

static void vec4_get_value_string(const vvp_vector4_t&word_val, unsigned width,
				  s_vpi_value*vp);

void vpip_vec4_get_value(const vvp_vector4_t&word_val, unsigned width,
			 bool signed_flag, s_vpi_value*vp)
{
      char *rbuf = 0;

      switch (vp->format) {
	  default:
	    fprintf(stderr, "internal error: Format %d not implemented\n",
		    vp->format);
	    assert(0 && "format not implemented");

	  case vpiBinStrVal:
	    rbuf = need_result_buf(width+1, RBUF_VAL);
	    for (unsigned idx = 0 ;  idx < width ;  idx += 1) {
		  vvp_bit4_t bit = word_val.value(idx);
		  rbuf[width-idx-1] = "01xz"[bit];
	    }
	    rbuf[width] = 0;
	    vp->value.str = rbuf;
	    break;

	  case vpiOctStrVal: {
		unsigned hwid = (width+2) / 3;
		rbuf = need_result_buf(hwid+1, RBUF_VAL);
		vpip_vec4_to_oct_str(word_val, rbuf, hwid+1, signed_flag);
		vp->value.str = rbuf;
		break;
	  }

	  case vpiDecStrVal: {
		rbuf = need_result_buf(width+1, RBUF_VAL);
		vpip_vec4_to_dec_str(word_val, rbuf, width+1, signed_flag);
		vp->value.str = rbuf;
		break;
	  }

	  case vpiHexStrVal: {
		unsigned  hwid = (width + 3) / 4;

		rbuf = need_result_buf(hwid+1, RBUF_VAL);
		rbuf[hwid] = 0;

		vpip_vec4_to_hex_str(word_val, rbuf, hwid+1, signed_flag);
		vp->value.str = rbuf;
		break;
	  }

          case vpiScalarVal: {
	        // scalars should be of size 1
	        assert(width == 1);
	        switch(word_val.value(0)) {
		    case BIT4_0:
		      vp->value.scalar = vpi0;
		      break;
	            case BIT4_1:
                      vp->value.scalar = vpi1;
                      break;
	            case BIT4_X:
                      vp->value.scalar = vpiX;
	            case BIT4_Z:
                      vp->value.scalar = vpiZ;
                      break;
		}
                break;
          }

	  case vpiIntVal: {
		long val = 0;
		vvp_bit4_t pad = BIT4_0;
		if (signed_flag && word_val.size() > 0)
		      pad = word_val.value(word_val.size()-1);

		for (unsigned idx = 0 ; idx < 8*sizeof(val) ;  idx += 1) {
		      vvp_bit4_t val4 = pad;
		      if (idx < word_val.size())
			    val4 = word_val.value(idx);
		      if (val4 == BIT4_1)
			    val |= 1L << idx;
		}

		vp->value.integer = val;
		break;
	  }

	  case vpiVectorVal: {
		unsigned hwid = (width - 1)/32 + 1;

		rbuf = need_result_buf(hwid * sizeof(s_vpi_vecval), RBUF_VAL);
		s_vpi_vecval *op = (p_vpi_vecval)rbuf;
		vp->value.vector = op;

		op->aval = op->bval = 0;
		for (unsigned idx = 0 ;  idx < width ;  idx += 1) {
		      switch (word_val.value(idx)) {
			  case BIT4_0:
			    op->aval &= ~(1 << idx % 32);
			    op->bval &= ~(1 << idx % 32);
			    break;
			  case BIT4_1:
			    op->aval |=  (1 << idx % 32);
			    op->bval &= ~(1 << idx % 32);
			    break;
			  case BIT4_X:
			    op->aval |= (1 << idx % 32);
			    op->bval |= (1 << idx % 32);
			    break;
			  case BIT4_Z:
			    op->aval &= ~(1 << idx % 32);
			    op->bval |=  (1 << idx % 32);
			    break;
		      }
		      if (!((idx+1) % 32) && (idx+1 < width)) {
			    op++;
			    op->aval = op->bval = 0;
		      }
		}
		break;
	  }

	  case vpiStringVal:
	    vec4_get_value_string(word_val, width, vp);
	    break;

	  case vpiRealVal: {
		vector4_to_value(word_val, vp->value.real, signed_flag);
		break;
	  }
      }
}

static void vec4_get_value_string(const vvp_vector4_t&word_val, unsigned width,
				  s_vpi_value*vp)
{
      unsigned nchar = width / 8;
      unsigned tail = width % 8;

      char*rbuf = need_result_buf(nchar + 1, RBUF_VAL);
      char*cp = rbuf;

      if (tail > 0) {
	    char char_val = 0;
	    for (unsigned idx = width-tail; idx < width ;  idx += 1) {
		  vvp_bit4_t val = word_val.value(idx);
		  if (val == BIT4_1)
			char_val |= 1 << idx;
	    }

	    if (char_val != 0)
		  *cp++ = char_val;
      }

      for (unsigned idx = 0 ;  idx < nchar ;  idx += 1) {
	    unsigned bit = (nchar - idx - 1) * 8;
	    char char_val = 0;
	    for (unsigned bdx = 0 ;  bdx < 8 ;  bdx += 1) {
		  vvp_bit4_t val = word_val.value(bit+bdx);
		  if (val == BIT4_1)
			char_val |= 1 << bdx;
	    }
	    if (char_val != 0)
		  *cp++ = char_val;
      }

      *cp = 0;
      vp->value.str = rbuf;
}

void vpi_get_value(vpiHandle expr, s_vpi_value*vp)
{
      assert(expr);
      assert(vp);
      if (expr->vpi_type->vpi_get_value_) {
	    (expr->vpi_type->vpi_get_value_)(expr, vp);

	    if (vpi_trace) switch (vp->format) {
		case vpiStringVal:
		  fprintf(vpi_trace,"vpi_get_value(%p=<%d>) -> string=\"%s\"\n",
			  expr, expr->vpi_type->type_code, vp->value.str);
		  break;

		case vpiBinStrVal:
		  fprintf(vpi_trace, "vpi_get_value(<%d>...) -> binstr=%s\n",
			  expr->vpi_type->type_code, vp->value.str);
		  break;

		case vpiIntVal:
		  fprintf(vpi_trace, "vpi_get_value(<%d>...) -> int=%d\n",
			  expr->vpi_type->type_code, vp->value.integer);
		  break;

		default:
		  fprintf(vpi_trace, "vpi_get_value(<%d>...) -> <%d>=?\n",
			  expr->vpi_type->type_code, vp->format);
	    }
	    return;
      }

      if (vpi_trace) {
	    fprintf(vpi_trace, "vpi_get_value(<%d>...) -> <suppress>\n",
		    expr->vpi_type->type_code);
      }

      vp->format = vpiSuppressVal;
}

struct vpip_put_value_event : vvp_gen_event_s {
      vpiHandle handle;
      s_vpi_value value;
      virtual void run_run();
  ~vpip_put_value_event() { }
};

void vpip_put_value_event::run_run()
{
      handle->vpi_type->vpi_put_value_ (handle, &value);
}

vpiHandle vpi_put_value(vpiHandle obj, s_vpi_value*vp,
			s_vpi_time*when, PLI_INT32 flags)
{
      assert(obj);

      if (obj->vpi_type->vpi_put_value_ == 0)
	    return 0;

      if (flags != vpiNoDelay) {
	    vvp_time64_t dly;

	    assert(when != 0);

 	    switch (when->type) {
 		case vpiScaledRealTime:
 		  dly = (vvp_time64_t)(when->real *
				       (pow(10.0L,
					    vpip_time_units_from_handle(obj) -
					    vpip_get_time_precision())));
 		  break;
 		case vpiSimTime:
		  dly = vpip_timestruct_to_time(when);
 		  break;
 		default:
		  dly = 0;
		  break;
 	    }

	    vpip_put_value_event*put = new vpip_put_value_event;
	    put->handle = obj;
	    put->value = *vp;
	    schedule_generic(put, dly, false);
	    return 0;
      }

      (obj->vpi_type->vpi_put_value_)(obj, vp);

      return 0;
}

vpiHandle vpi_handle(PLI_INT32 type, vpiHandle ref)
{
      if (type == vpiSysTfCall) {
	    assert(ref == 0);

	    if (vpi_trace) {
		  fprintf(vpi_trace, "vpi_handle(vpiSysTfCall, 0) "
			  "-> %p (%s)\n", &vpip_cur_task->base,
			  vpip_cur_task->defn->info.tfname);
	    }

	    return &vpip_cur_task->base;
      }

      if (ref == 0) {
	    fprintf(stderr, "internal error: vpi_handle(type=%d, ref=0)\n",
		    type);
      }
      assert(ref);

      if (ref->vpi_type->handle_ == 0) {

	    if (vpi_trace) {
		  fprintf(vpi_trace, "vpi_handle(%d, %p) -X\n",
			  type, ref);
	    }

	    return 0;
      }

      assert(ref->vpi_type->handle_);
      vpiHandle res = (ref->vpi_type->handle_)(type, ref);

      if (vpi_trace) {
	    fprintf(vpi_trace, "vpi_handle(%d, %p) -> %p\n",
		    type, ref, res);
      }

      return res;
}

/*
 * This function asks the object to return an iterator for
 * the specified reference. It is up to the iterate_ method to
 * allocate a properly formed iterator.
 */
static vpiHandle vpi_iterate_global(int type)
{
      switch (type) {
	  case vpiModule:
	    return vpip_make_root_iterator();

      }

      return 0;
}

vpiHandle vpi_iterate(PLI_INT32 type, vpiHandle ref)
{
      vpiHandle rtn = 0;

      assert(vpi_mode_flag != VPI_MODE_NONE);
      if (vpi_mode_flag == VPI_MODE_REGISTER) {
	    fprintf(stderr, "vpi error: vpi_iterate called during "
		    "vpi_register_systf. You can't do that!\n");
	    return 0;
      }

      if (ref == 0)
	    rtn = vpi_iterate_global(type);
      else if (ref->vpi_type->iterate_)
	    rtn = (ref->vpi_type->iterate_)(type, ref);

      if (vpi_trace) {
	    fprintf(vpi_trace, "vpi_iterate(%d, %p) ->%s\n",
	    type, ref, rtn ? "" : " (null)");
      }

      return rtn;
}

vpiHandle vpi_handle_by_index(vpiHandle ref, PLI_INT32 idx)
{
      assert(ref);

      if (ref->vpi_type->index_ == 0)
	    return 0;

      assert(ref->vpi_type->index_);
      return (ref->vpi_type->index_)(ref, idx);
}

static vpiHandle find_name(const char *name, vpiHandle handle)
{
      vpiHandle rtn = 0;
      struct __vpiScope*ref = (struct __vpiScope*)handle;

      /* check module names */
      if (!strcmp(name, vpi_get_str(vpiName, handle)))
	    rtn = handle;

      /* brute force search for the name in all objects in this scope */
      for (unsigned i = 0 ;  i < ref->nintern ;  i += 1) {
	    char *nm = vpi_get_str(vpiName, ref->intern[i]);
	    if (!strcmp(name, nm)) {
		  rtn = ref->intern[i];
		  break;
	    } else if (vpi_get(vpiType, ref->intern[i]) == vpiMemory) {
		  /* We need to iterate on the words */
		  vpiHandle word_i, word_h;
		  word_i = vpi_iterate(vpiMemoryWord, ref->intern[i]);
		  while (word_i && (word_h = vpi_scan(word_i))) {
			nm = vpi_get_str(vpiName, word_h);
			if (!strcmp(name, nm)) {
			      rtn = word_h;
			      break;
			}
		  }
	    }
	    /* found it yet? */
	    if (rtn) break;
      }

      return rtn;
}

static vpiHandle find_scope(const char *name, vpiHandle handle, int depth)
{
      vpiHandle iter, hand, rtn = 0;

      iter = !handle ? vpi_iterate(vpiModule, NULL) :
		       vpi_iterate(vpiInternalScope, handle);

      while (iter && (hand = vpi_scan(iter))) {
	    char *nm = vpi_get_str(vpiName, hand);
	    int len = strlen(nm);
	    const char *cp = name + len;	/* hier separator */

	    if (!handle && !strcmp(name, nm)) {
		  /* root module */
		  rtn = hand;
	    } else if (!strncmp(name, nm, len) && *(cp) == '.')
		  /* recurse deeper */
		  rtn = find_scope(cp+1, hand, depth + 1);

	    /* found it yet ? */
	    if (rtn) break;
      }

      /* matched up to here */
      if (!rtn) rtn = handle;

      return rtn;
}

vpiHandle vpi_handle_by_name(const char *name, vpiHandle scope)
{
      vpiHandle hand;
      const char *nm, *cp;
      int len;


      if (vpi_trace) {
	    fprintf(vpi_trace, "vpi_handle_by_name(%s, %p) -->\n",
		    name, scope);
      }

      /* If scope provided, look in corresponding module; otherwise
       * traverse the hierarchy specified in name to find the leaf module
       * and try finding it there.
       */
      if (scope) {
	    /* Some implementations support either a module or a scope. */
	    if (vpi_get(vpiType, scope ) == vpiScope) {
	          hand = vpi_handle(vpiModule, scope);
	    } else {
	          hand = scope;
	    }
      } else {
	    hand = find_scope(name, NULL, 0);
      }

      if (hand) {
	    /* remove hierarchical portion of name */
	    nm = vpi_get_str(vpiFullName, hand);
	    len = strlen(nm);
	    cp = name + len;
	    if (!strncmp(name, nm, len) && *cp == '.') name = cp + 1;

	    /* Ok, time to burn some cycles */
	    vpiHandle out = find_name(name, hand);
	    return out;
      }

      return 0;
}

extern "C" PLI_INT32 vpi_vprintf(const char*fmt, va_list ap)
{
      return vpi_mcd_vprintf(1, fmt, ap);
}

extern "C" PLI_INT32 vpi_printf(const char *fmt, ...)
{
      va_list ap;
      va_start(ap, fmt);
      int r = vpi_mcd_vprintf(1, fmt, ap);
      va_end(ap);
      return r;
}

extern "C" PLI_INT32 vpi_flush(void)
{
      return vpi_mcd_flush(1);
}


extern "C" void vpi_sim_vcontrol(int operation, va_list ap)
{
      long diag_msg;

      switch (operation) {
	  case vpiFinish:
            diag_msg = va_arg(ap, long);
	    schedule_finish(diag_msg);
	    break;

	  case vpiStop:
            diag_msg = va_arg(ap, long);
	    schedule_stop(diag_msg);
	    break;

	  default:
	    fprintf(stderr, "Unsupported operation %d.\n", operation);
	    assert(0);
      }
}

extern "C" void vpi_sim_control(PLI_INT32 operation, ...)
{
      va_list ap;
      va_start(ap, operation);
      vpi_sim_vcontrol(operation, ap);
      va_end(ap);
}

extern "C" void vpi_control(PLI_INT32 operation, ...)
{
      va_list ap;
      va_start(ap, operation);
      vpi_sim_vcontrol(operation, ap);
      va_end(ap);
}

