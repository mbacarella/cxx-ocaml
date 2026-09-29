module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
module type T = sig type b val w : int end
let f x = let (module X : T with type b = int) = x in 0
