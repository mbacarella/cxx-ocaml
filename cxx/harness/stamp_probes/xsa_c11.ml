module F (H : Hashtbl.HashedType) = struct module W = Weak.Make(H) end
