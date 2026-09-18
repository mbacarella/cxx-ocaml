module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
module N : sig class type c = object method a : int M.s -> int end end =
  struct class type c = object method a : int M.s -> int end end
