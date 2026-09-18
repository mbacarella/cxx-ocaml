module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
let f x = match x with (module X : M.S) -> 0
