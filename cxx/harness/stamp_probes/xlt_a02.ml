module Id (X : sig type t end) = struct type t = X.t end
module G (X : sig type t end) = struct
  type u = Id (X).t
end
