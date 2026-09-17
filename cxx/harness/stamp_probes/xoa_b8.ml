module M = struct
  module F (X : sig type t end) = struct
    type u = X.t list
    let f (x : u) = x
  end
end
module P = struct type t = int end
open M.F(P)
