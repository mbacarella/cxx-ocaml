module F (X : sig end) = struct
  class type t = object end
  class c = object end
end;;
module M1 = struct end;;
module H (X : sig end) (Y : sig end) = struct class type t = object end end;;
class type u = H(M1)(M1).t;;
