module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class b = object method a : int M.s -> int = fun x -> 0 end
class d = object inherit b as super method z = super#a end
