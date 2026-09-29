module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
class b = object method a : 'a. 'a M.s -> 'a = fun (type b) ((module X) :
  (module M.S with type a = b)) -> X.v end
