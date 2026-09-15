module F (X : Set.OrderedType) = struct
  type t = Set.Make(X).t
  type 'a u = 'a Map.Make(X).t
end
