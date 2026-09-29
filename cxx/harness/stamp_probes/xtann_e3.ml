module type S = sig module M : sig type t end end
module A : S = struct module M = struct type t = int end end
module F (X : S) = X.M
let f x = (x : F(A).t)
