module M = struct module type S = sig type a val v : a end
  type 'a s = (module S with type a = 'a) end
module type T = sig type t = int M.s end
module F (X : T) = struct type u = X.t end
