(* parser.mly's literal Lident constants (mkexp_cons / mkpat_cons "::",
   mktailexp "[]", the inlined constr_extra_ident "(::)", begin end, M.()) and
   location.ml's Location.none (mknoloc, ghost opens):
   one block each in a .cmi's attribute payloads (a signature
   constraint's, which the .cmi keeps) *)
module M : sig
  val a : int [@@foo [1; 2; 3]]
  val b : int [@@foo 1 :: 2 :: []]
  val c : int [@@foo begin end] [@@bar begin end]
  val f : int [@@foo (), ()]
  val g : int [@@foo [], []]
  val i : int [@@foo fun [x; y] -> x :: y]
  val j : int [@@foo fun (x :: _) -> x]
  val l : int [@@foo fun List.() -> ()]
  val m : int [@@foo (::), [], ()] [@@bar [[]; [[]]]]
  val n : int [@@foo (::) (1, [])] [@@bar fun [x] -> x]
  val o : int [@@foo fun (::) -> 1] [@@bar [1]]
end = struct
  let a = 0
  let b = 0
  let c = 0
  let f = 0
  let g = 0
  let i = 0
  let j = 0
  let l = 0
  let m = 0
  let n = 0
  let o = 0
end
