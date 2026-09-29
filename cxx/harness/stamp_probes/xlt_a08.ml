module Id (X : sig type t end) = X
module G (X : sig type t end) = struct
  type u = Id (X).t
end
module H (X : sig type t end) = struct
  type u = Id (X).t
end
