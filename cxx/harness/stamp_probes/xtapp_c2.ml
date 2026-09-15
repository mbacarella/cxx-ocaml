module F (X : Set.OrderedType) : sig val x : Set.Make(X).t end = struct let x = Obj.magic 0 end
