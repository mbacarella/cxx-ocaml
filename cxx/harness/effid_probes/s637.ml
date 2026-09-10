(* S421 CONTROL: a source-level `module Set = M.Set` really is Mp_absent on
   both sides -- it takes no field and the consumer re-derives it from M.  The
   fix must not make every alias present. *)
module F (M : sig
            type t
            module Set : Set.S with type elt = t
          end) =
struct
  let test set = Printf.printf "%d\n" (M.Set.cardinal set)
end

module M = F (S637n)

let () = M.test (S637n.Set.singleton "42")
