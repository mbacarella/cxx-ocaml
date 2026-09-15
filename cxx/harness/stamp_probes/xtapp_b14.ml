module F (X : Hashtbl.HashedType) = struct type 'a t = 'a Hashtbl.Make(X).t type u = Hashtbl.Make(X).key end
