module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
module N = struct module type T = sig type b val w : int end end
let f (module X : N.T) = X.w
let g (module X : N.T) = X.w
