module Id (X : sig type t type s end) = X
module G (X : sig type t type s end) = struct
  type u = Id (X).t
end
