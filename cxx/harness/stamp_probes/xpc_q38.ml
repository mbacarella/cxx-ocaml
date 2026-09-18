module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class b = object val x : int M.s = assert false method a = x end
class d = object inherit b end
