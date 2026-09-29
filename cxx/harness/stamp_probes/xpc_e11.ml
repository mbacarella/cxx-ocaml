module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
module B = struct class type a = object method a : 'a. 'a M.s -> 'a end end
module M' = M module B' = B
let f = fun (type a) ((module X) : (module M.S with type a = a)) -> X.v
