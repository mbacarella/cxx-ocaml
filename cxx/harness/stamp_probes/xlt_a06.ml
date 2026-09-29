module Id (X : sig type t end) = X
module G (X : sig type t end) = struct
  let f (x : Id (X).t) = x
end
