module G (X : sig end) = struct type 'a t = 'a list end;; module M1 = struct
end;; module N : sig val f : int G(M1).t -> int G(M1).t end = struct
let f (x : int G(M1).t) = x end;;
