module type S = sig module M : sig type t end end
module A : S = struct module M = struct type t = int end end
module F (X : S) = struct type t = X.M.t end
let f (x : F(A).t) = x
