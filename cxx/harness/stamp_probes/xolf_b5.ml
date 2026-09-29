module F (X : sig type t end) = struct type u = U of X.t end
module P = struct type t = int end
module M = struct open F(P) let c (U y) = y end
