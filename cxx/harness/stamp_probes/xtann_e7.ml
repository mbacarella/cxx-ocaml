module F (X : Set.OrderedType) = struct let x : Set.Make(X).t = Obj.magic 0 end
