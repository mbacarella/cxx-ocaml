module M = struct module type S = sig type a val v : a end end
let f (type a) ((module X) : (module M.S with type a = a)) = X.v
