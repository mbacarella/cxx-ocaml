module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class b = object method a = fun (x : int M.s) (y : int M.s) -> 0 method c
  = fun (z : int M.s) -> 1 end
