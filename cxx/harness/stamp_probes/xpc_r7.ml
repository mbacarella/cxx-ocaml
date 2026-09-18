module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class b = object method a : int -> int M.s -> int = fun y (x : int M.s) -> 0 end
