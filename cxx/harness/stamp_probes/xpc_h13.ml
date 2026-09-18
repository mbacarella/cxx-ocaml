module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
module type T = sig type b type c end
let f = fun ((module X) : (module T with type b = int and type c = int)) -> 0
