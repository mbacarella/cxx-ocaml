module type NI = sig type t end
module type I = sig type t [@@immediate] end
module Make(Im : I)(Ni : NI) : sig
  type t [@@immediate64] type 'a repr = Im : Im.t repr | Ni : Ni.t repr
  val repr : t repr
end = struct type t = int type 'a repr = Im : Im.t repr | Ni : Ni.t repr
  let repr = Obj.magic 0 end
