(* FUZZ: levels=nat,fl flags=fl:-O3
   Found by camlsmith port mode (night1 campaign, seed 4): the .cmx of a
   unit whose exported function bodies use atomic record fields.  Two
   sharings of ocamlopt's heap that Marshal keeps:
   - Translcore's [%atomic.loc] block shape is [kind; Pintval], i.e.
     kind :: the static literal [Pintval], so every such shape (closure's
     Ulambda bodies) shares its last cell;
   - Translcore's atomic `r.f <- v` evaluates Primitive.simple at each use:
     a fresh description per assignment whose name is one string literal
     (the port had one static description; flambda's .cmx showed it). *)
type r = { mutable x : int [@atomic]; mutable y : string [@atomic]; z : int }
let[@inline never] add (l : int Atomic.Loc.t) n = Atomic.Loc.fetch_and_add l n
let[@inline never] swap (l : string Atomic.Loc.t) s = Atomic.Loc.exchange l s
let[@inline] f r = add [%atomic.loc r.x] 1 + add [%atomic.loc r.x] 2
let[@inline] g r = swap [%atomic.loc r.y] "a" ^ swap [%atomic.loc r.y] "b"
let[@inline] h r = r.x <- 3; r.y <- "c"; r.x <- r.z; r.x
let () =
  let r = { x = 1; y = ""; z = 7 } in
  Printf.printf "%d %s %d\n" (f r) (g r) (h r)
