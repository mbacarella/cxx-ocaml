module F (X : sig type t end) = struct type u = X.t list let f (x : u) = x end
module P = struct type t = int end
module N = struct open F(P) type r = u end
