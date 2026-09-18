module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
let f = function ((module X) : (module M.S with type a = int)) -> 0
