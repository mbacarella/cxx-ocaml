module F (X : Hashtbl.HashedType) = struct type 'a t = 'a Hashtbl.Make(X).t end
