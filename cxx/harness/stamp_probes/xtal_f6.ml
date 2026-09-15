module F (X : sig type t val compare : t -> t -> int end) =
struct
  type t = Set.Make(X).t
end
