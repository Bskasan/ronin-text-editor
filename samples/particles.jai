// particles.jai: a tiny particle system, stepped on the CPU.
#import "Basic";
#import "Math";

MAX_PARTICLES :: 256;
GRAVITY       :: 9.81;

Particle :: struct {
    position: Vector2;
    velocity: Vector2;
    life:     float = 1.0;
    alive:    bool;
}

/* Comments nest in Jai:
   /* this one is inside */ and the outer one goes on. */
update :: (particles: [] Particle, dt: float) -> s32 {
    alive_count: s32 = 0;
    for * p: particles {
        if !p.alive  continue;
        p.velocity.y -= GRAVITY * dt;
        p.position   += p.velocity * dt;
        p.life       -= dt * 0.5;
        if p.life <= 0 {
            p.alive = false;
        } else {
            alive_count += 1;
        }
    }
    return alive_count;
}

main :: () {
    particles: [MAX_PARTICLES] Particle;
    for * particles  it.alive = it_index % 2 == 0;
    names: [4] string = ---;
    count := update(particles, 1.0 / 60.0);
    print("% particles alive (mask %)\n", count, 0xC0FF_EE);
}
