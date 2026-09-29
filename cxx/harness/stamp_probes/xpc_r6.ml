module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class b = object method a : int M.s -> int = fun (type b) ((module X) :
  (module M.S with type a = int)) -> X.v end
