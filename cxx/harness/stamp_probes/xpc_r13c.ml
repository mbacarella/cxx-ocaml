module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class b = object method a : int -> int M.s = fun x : int M.s -> assert false end
