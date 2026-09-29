module Id (X : sig type t end) (Y : sig type s end) = X
module G (X : sig type t end) (Y : sig type s end) = struct
  type u = Id (X) (Y).t
end
