module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class type c = object method a : int M.s -> int end
class type d = object inherit c end
