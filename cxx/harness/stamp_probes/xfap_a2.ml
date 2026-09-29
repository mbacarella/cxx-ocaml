module Q (S : sig type t end) = struct type u = S.t list let x : u = [] end
module P (B : sig type t end) = Q (B)
