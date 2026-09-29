module F (X : sig end) = struct class type t = object end class c = object end
end;; module M1 = struct end;; module G (X : sig type t end) = struct class
type t = object method m : X.t end end;; module M2 = struct type t = int
type s = int end;; class type u = G(M2).t;;
