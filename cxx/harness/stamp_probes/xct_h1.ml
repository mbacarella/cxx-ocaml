module F (X : sig end) = struct class type t = object end class c = object end
end;; module M1 = struct end;; module H (P : sig module F (X : sig
end) : sig class type t = object end end end) = struct class type u = P.F(M1).t
end;;
