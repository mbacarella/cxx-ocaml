module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class b = object method a : (module M.S with type a = int) -> int = fun
  (module X : M.S with type a = int) -> 0 end
