module F (X : Set.OrderedType) : sig val x : Set.Make(X).t end = struct module S = Set.Make(X) let x = Obj.magic 0 end
