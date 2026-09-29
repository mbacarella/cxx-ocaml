module F (X : Map.OrderedType) = struct
  type 'a t = 'a Map.Make(X).t
  type s = Set.Make(X).t
end
