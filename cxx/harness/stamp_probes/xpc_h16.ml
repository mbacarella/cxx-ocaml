module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
module type T = sig type b type c val w : int end
let f = fun ((module X) : (module T)) -> 0
